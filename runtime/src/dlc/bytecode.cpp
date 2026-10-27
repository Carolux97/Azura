// UnrealScript bytecode assembler. See bytecode.h.

#include "bytecode.h"

#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace azura::dlc::bytecode {
namespace {

// Tokens.
constexpr uint8_t EX_LOCAL_VARIABLE = 0x00;
constexpr uint8_t EX_INSTANCE_VARIABLE = 0x01;
constexpr uint8_t EX_RETURN = 0x04;
constexpr uint8_t EX_JUMP = 0x06;
constexpr uint8_t EX_JUMP_IF_NOT = 0x07;
constexpr uint8_t EX_STOP = 0x08;
constexpr uint8_t EX_NOTHING = 0x0B;
constexpr uint8_t EX_LABEL_TABLE = 0x0C;
constexpr uint8_t EX_LET = 0x0F;
constexpr uint8_t EX_DYN_ARRAY_ELEMENT = 0x10;
constexpr uint8_t EX_NEW = 0x11;
constexpr uint8_t EX_LET_BOOL = 0x14;
constexpr uint8_t EX_END_FUNCTION_PARMS = 0x16;
constexpr uint8_t EX_SELF = 0x17;
constexpr uint8_t EX_SKIP = 0x18;
constexpr uint8_t EX_CONTEXT = 0x19;
constexpr uint8_t EX_VIRTUAL_FUNCTION = 0x1B;
constexpr uint8_t EX_FINAL_FUNCTION = 0x1C;
constexpr uint8_t EX_INT_CONST = 0x1D;
constexpr uint8_t EX_FLOAT_CONST = 0x1E;
constexpr uint8_t EX_OBJECT_CONST = 0x20;
constexpr uint8_t EX_NAME_CONST = 0x21;
constexpr uint8_t EX_INT_ONE = 0x25;
constexpr uint8_t EX_TRUE = 0x27;
constexpr uint8_t EX_FALSE = 0x28;
constexpr uint8_t EX_NO_OBJECT = 0x2A;
constexpr uint8_t EX_BOOL_VARIABLE = 0x2D;
constexpr uint8_t EX_STRUCT_MEMBER = 0x35;
constexpr uint8_t EX_DYNAMIC_CAST = 0x2E;
constexpr uint8_t EX_DYN_ARRAY_LENGTH = 0x36;
constexpr uint8_t EX_RETURN_NOTHING = 0x3A;
constexpr uint8_t EX_EMPTY_PARM_VALUE = 0x4A;
constexpr uint8_t AND_AND_BOOL_BOOL = 0x82;
constexpr uint8_t EX_END_OF_SCRIPT = 0x53;

std::vector<uint8_t> Be32(uint32_t v) { return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }

Code Seq(std::initializer_list<Code> args) {
  Code out;
  for (const Code& c : args) out += c;
  return out;
}

}  // namespace

Code::Code(std::initializer_list<Code> codes) {
  for (const Code& c : codes) *this += c;
}

Code& Code::operator+=(const Code& other) {
  parts_.insert(parts_.end(), other.parts_.begin(), other.parts_.end());
  return *this;
}

uint32_t Code::MemorySize() const {
  uint32_t size = 0;
  for (const Part& part : parts_) {
    if (part.kind == Part::kJump) size += 2;
    if (part.kind == Part::kRaw) size += part.memory_size;
  }
  return size;
}

Code Code::Raw(std::vector<uint8_t> bytes, uint32_t memory_size) {
  Code c;
  c.parts_.push_back({Part::kRaw, std::move(bytes), memory_size, {}});
  return c;
}

Code Code::Label(std::string text) {
  Code c;
  c.parts_.push_back({Part::kLabel, {}, 0, std::move(text)});
  return c;
}

Code Code::Target(std::string label) {
  Code c;
  c.parts_.push_back({Part::kJump, {}, 0, std::move(label)});
  return c;
}

Code Label(const std::string& text) { return Code::Label(text); }

Code U8(uint8_t v) { return Code::Raw({v}, 1); }

Code Ref(int32_t index) { return Code::Raw(Be32(uint32_t(index)), 8); }

Code Name(int32_t index) {
  std::vector<uint8_t> bytes = Be32(uint32_t(index));
  bytes.insert(bytes.end(), 4, 0);  // number 0
  return Code::Raw(bytes, 8);
}

Code Local(int32_t property) { return {U8(EX_LOCAL_VARIABLE), Ref(property)}; }
Code Instance(int32_t property) { return {U8(EX_INSTANCE_VARIABLE), Ref(property)}; }
Code BoolVariable(const Code& variable) { return {U8(EX_BOOL_VARIABLE), variable}; }
Code Let(const Code& lhs, const Code& rhs) { return {U8(EX_LET), lhs, rhs}; }
Code LetBool(const Code& lhs, const Code& rhs) { return {U8(EX_LET_BOOL), lhs, rhs}; }
Code Cast(int32_t cls, const Code& expr) { return {U8(EX_DYNAMIC_CAST), Ref(cls), expr}; }
Code NoObject() { return U8(EX_NO_OBJECT); }
Code TrueConst() { return U8(EX_TRUE); }
Code FalseConst() { return U8(EX_FALSE); }
Code IntConst(int32_t v) { return {U8(EX_INT_CONST), Code::Raw(Be32(uint32_t(v)), 4)}; }
Code IntOne() { return U8(EX_INT_ONE); }

Code FloatConst(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  return {U8(EX_FLOAT_CONST), Code::Raw(Be32(bits), 4)};
}

Code NameConst(int32_t name) { return {U8(EX_NAME_CONST), Name(name)}; }
Code Self() { return U8(EX_SELF); }
Code Nothing() { return U8(EX_NOTHING); }
Code EmptyParm() { return U8(EX_EMPTY_PARM_VALUE); }

Code New(int32_t cls) {
  return {U8(EX_NEW), Nothing(), Nothing(), Nothing(), U8(EX_OBJECT_CONST), Ref(cls), Nothing()};
}

Code Context(const Code& obj, const Code& member, int32_t result, bool call_only) {
  for (const auto& part : member.parts()) {
    if (part.kind != Code::Part::kRaw) throw std::logic_error("no labels inside a context");
  }
  const uint32_t skip = call_only ? 1 + 8 : member.MemorySize();
  return {U8(EX_CONTEXT), obj, Code::Raw({uint8_t(skip >> 8), uint8_t(skip)}, 2), Ref(result), U8(0), member};
}

Code Call(int32_t function_name, std::initializer_list<Code> args) {
  return {U8(EX_VIRTUAL_FUNCTION), Name(function_name), Seq(args), U8(EX_END_FUNCTION_PARMS)};
}

Code CallFinal(int32_t function, std::initializer_list<Code> args) {
  return {U8(EX_FINAL_FUNCTION), Ref(function), Seq(args), U8(EX_END_FUNCTION_PARMS)};
}

Code Native(uint8_t token, std::initializer_list<Code> args) {
  return {U8(token), Seq(args), U8(EX_END_FUNCTION_PARMS)};
}

Code StructMember(int32_t member, int32_t struct_, const Code& expr) {
  return {U8(EX_STRUCT_MEMBER), Ref(member), Ref(struct_), U8(0), U8(1), expr};
}

Code AndAlso(const Code& a, const Code& b) {
  const uint32_t skip = b.MemorySize() + 1;
  return {U8(AND_AND_BOOL_BOOL), a, U8(EX_SKIP), Code::Raw({uint8_t(skip >> 8), uint8_t(skip)}, 2), b,
          U8(EX_END_FUNCTION_PARMS)};
}

Code ArrayElement(const Code& index, const Code& array) { return {U8(EX_DYN_ARRAY_ELEMENT), index, array}; }
Code ArrayLength(const Code& array) { return {U8(EX_DYN_ARRAY_LENGTH), array}; }

Code JumpIfNot(const std::string& to, const Code& condition) {
  return {U8(EX_JUMP_IF_NOT), Code::Target(to), condition};
}

Code Jump(const std::string& to) { return {U8(EX_JUMP), Code::Target(to)}; }
Code Return() { return {U8(EX_RETURN), Nothing()}; }
Code Return(const Code& value) { return {U8(EX_RETURN), value}; }
Code ReturnNothing(int32_t result) { return {U8(EX_RETURN), U8(EX_RETURN_NOTHING), Ref(result)}; }
Code Stop() { return U8(EX_STOP); }
Code End() { return U8(EX_END_OF_SCRIPT); }

std::pair<std::vector<uint8_t>, uint32_t> Assemble(const Code& code) {
  std::unordered_map<std::string, uint32_t> labels;
  uint32_t offset = 0;
  for (const auto& part : code.parts()) {
    if (part.kind == Code::Part::kLabel) labels[part.label] = offset;
    if (part.kind == Code::Part::kJump) offset += 2;
    if (part.kind == Code::Part::kRaw) offset += part.memory_size;
  }
  std::vector<uint8_t> out;
  for (const auto& part : code.parts()) {
    if (part.kind == Code::Part::kJump) {
      const auto it = labels.find(part.label);
      if (it == labels.end()) throw std::logic_error("jump to an unknown label: " + part.label);
      out.push_back(uint8_t(it->second >> 8));
      out.push_back(uint8_t(it->second));
    } else if (part.kind == Code::Part::kRaw) {
      out.insert(out.end(), part.bytes.begin(), part.bytes.end());
    }
  }
  return {out, offset};
}

AssembledState AssembleState(Code code, const std::vector<std::pair<std::string, int32_t>>& labels, int32_t none) {
  while ((code.MemorySize() + 1) % 4) code += Nothing();
  AssembledState out;
  out.label_table = uint16_t(code.MemorySize() + 1);
  std::unordered_map<std::string, uint32_t> offsets;
  uint32_t offset = 0;
  for (const auto& part : code.parts()) {
    if (part.kind == Code::Part::kLabel) offsets[part.label] = offset;
    if (part.kind == Code::Part::kJump) offset += 2;
    if (part.kind == Code::Part::kRaw) offset += part.memory_size;
  }
  Code table = U8(EX_LABEL_TABLE);
  for (const auto& [text, name] : labels) {
    const auto it = offsets.find(text);
    if (it == offsets.end()) throw std::logic_error("no label " + text);
    table += Name(name);
    table += Code::Raw(Be32(it->second), 4);
  }
  table += Name(none);
  table += Code::Raw(Be32(0xFFFF), 4);
  code += table;
  code += End();
  auto [stored, size] = Assemble(code);
  out.code = std::move(stored);
  out.memory_size = size;
  return out;
}

}  // namespace azura::dlc::bytecode

// A small assembler for UnrealScript bytecode (Unreal Engine 3, cooked for Xbox 360, big-endian), for
// the functions of Azura's DLC script patch. The C++ version of tools/dlc/bytecode.py.
//
// A function's bytecode is stored with 4-byte object references, but the engine loads it with 8-byte
// ones; jump targets and the skip sizes of context expressions count bytes of that loaded form. Code
// here is built from fragments that know both sizes, and labels are resolved when the function is
// assembled.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace azura::dlc::bytecode {

// Native functions and operators (their index is their token).
constexpr uint8_t GOTO_STATE = 0x71;
constexpr uint8_t NOT_EQUAL_OBJECT_OBJECT = 0x77;
constexpr uint8_t NOT_PRE_BOOL = 0x81;
constexpr uint8_t LESS_INT_INT = 0x96;
constexpr uint8_t GREATER_INT_INT = 0x97;
constexpr uint8_t SUBTRACT_FLOAT_FLOAT = 0xAF;
constexpr uint8_t GREATER_FLOAT_FLOAT = 0xB1;
constexpr uint8_t ADD_ADD_INT = 0xA5;            // i++
constexpr uint8_t SUBTRACT_SUBTRACT_INT = 0xA6;  // i--

// A run of bytecode: (bytes, size in memory) parts, labels and jumps to labels.
class Code {
 public:
  struct Part {
    enum Kind { kRaw, kLabel, kJump } kind;
    std::vector<uint8_t> bytes;  // kRaw
    uint32_t memory_size = 0;    // kRaw
    std::string label;           // kLabel, kJump
  };

  Code() = default;
  Code(std::initializer_list<Code> codes);
  Code& operator+=(const Code& other);
  uint32_t MemorySize() const;
  const std::vector<Part>& parts() const { return parts_; }

  static Code Raw(std::vector<uint8_t> bytes, uint32_t memory_size);
  static Code Label(std::string text);
  // A jump's operand: the memory offset of a label (2 bytes).
  static Code Target(std::string label);

 private:
  std::vector<Part> parts_;
};

Code U8(uint8_t v);
// An object reference: 4 bytes stored, 8 in memory.
Code Ref(int32_t index);
Code Name(int32_t index);
Code Label(const std::string& text);

// Expressions and statements.
Code Local(int32_t property);
Code Instance(int32_t property);
Code BoolVariable(const Code& variable);
Code Let(const Code& lhs, const Code& rhs);
Code LetBool(const Code& lhs, const Code& rhs);
Code Cast(int32_t cls, const Code& expr);
Code NoObject();
Code TrueConst();
Code FalseConst();
Code IntConst(int32_t v);
Code FloatConst(float v);
Code NameConst(int32_t name);
Code Self();
Code IntOne();
Code Nothing();
Code EmptyParm();
// new class'<cls>' (no outer, name, flags or template).
Code New(int32_t cls);
// obj.member; `result` is the property that holds the member's value (0 for none), which the engine
// clears when obj is None, skipping the member's bytecode. For a call on Outer (`call_only`), the
// script compiler's skip covers only the call's token and name.
Code Context(const Code& obj, const Code& member, int32_t result = 0, bool call_only = false);
// A virtual function call by name.
Code Call(int32_t function_name, std::initializer_list<Code> args);
Code CallFinal(int32_t function, std::initializer_list<Code> args);
Code Native(uint8_t token, std::initializer_list<Code> args);
// expr.member, a member of a struct value (`struct_` its struct), here assigned to.
Code StructMember(int32_t member, int32_t struct_, const Code& expr);
// a && b: the second operand is skipped (with the end of the parameters) when a is false.
Code AndAlso(const Code& a, const Code& b);
Code ArrayElement(const Code& index, const Code& array);
Code ArrayLength(const Code& array);
Code JumpIfNot(const std::string& to, const Code& condition);
Code Jump(const std::string& to);
Code Return();
Code Return(const Code& value);
// The end of a function that returns a value: returns `result` (its ReturnValue) as it is.
Code ReturnNothing(int32_t result);
Code Stop();
Code End();

// (stored bytecode, its size in memory), with each jump's label resolved.
std::pair<std::vector<uint8_t>, uint32_t> Assemble(const Code& code);

// A state's code: `code`, then its label table (each of `labels`, (label, name index), at its offset,
// then None), aligned to 4 bytes in memory with Nothing tokens, as the compiler writes it.
struct AssembledState {
  std::vector<uint8_t> code;
  uint32_t memory_size = 0;
  uint16_t label_table = 0;  // the label table's offset in memory
};
AssembledState AssembleState(Code code, const std::vector<std::pair<std::string, int32_t>>& labels, int32_t none);

}  // namespace azura::dlc::bytecode

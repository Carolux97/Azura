"""A small assembler for UnrealScript bytecode (Unreal Engine 3, cooked for Xbox 360, big-endian).

A function's bytecode is stored with 4-byte object references, but the engine loads it with 8-byte
ones; jump targets and the skip sizes of context expressions count bytes of that loaded form. Code
here is built from fragments that know both sizes, and labels are resolved when the function is
assembled.

    code = (let(local(lp), cast(local_player_class, instance(player))) +
            jump_if_not('end', native(NOT_EQUAL_OBJECT, local(lp), none())) + ... +
            label('end') + ret() + end())
    stored, memory_size = assemble(code)
"""

import struct

# Tokens.
EX_LOCAL_VARIABLE = 0x00
EX_INSTANCE_VARIABLE = 0x01
EX_RETURN = 0x04
EX_JUMP = 0x06
EX_JUMP_IF_NOT = 0x07
EX_STOP = 0x08
EX_NOTHING = 0x0B
EX_LABEL_TABLE = 0x0C
EX_LET = 0x0F
EX_DYN_ARRAY_ELEMENT = 0x10
EX_NEW = 0x11
EX_LET_BOOL = 0x14
EX_END_FUNCTION_PARMS = 0x16
EX_SELF = 0x17
EX_SKIP = 0x18
EX_CONTEXT = 0x19
EX_VIRTUAL_FUNCTION = 0x1B
EX_FINAL_FUNCTION = 0x1C
EX_INT_CONST = 0x1D
EX_FLOAT_CONST = 0x1E
EX_NAME_CONST = 0x21
EX_INT_ONE = 0x25
EX_TRUE = 0x27
EX_FALSE = 0x28
EX_NO_OBJECT = 0x2A
EX_BOOL_VARIABLE = 0x2D
EX_STRUCT_MEMBER = 0x35
EX_DYNAMIC_CAST = 0x2E
EX_DYN_ARRAY_LENGTH = 0x36
EX_RETURN_NOTHING = 0x3A
EX_EMPTY_PARM_VALUE = 0x4A
EX_END_OF_SCRIPT = 0x53

# Native functions and operators (their index is their token).
GOTO_STATE = 0x71
NOT_EQUAL_OBJECT_OBJECT = 0x77
NOT_PRE_BOOL = 0x81
AND_AND_BOOL_BOOL = 0x82
LESS_INT_INT = 0x96
GREATER_INT_INT = 0x97
SUBTRACT_FLOAT_FLOAT = 0xAF
GREATER_FLOAT_FLOAT = 0xB1
ADD_ADD_INT = 0xA5           # i++
SUBTRACT_SUBTRACT_INT = 0xA6  # i--


class Code:
    """A run of bytecode: (bytes, size in memory) parts, labels and jumps to labels."""

    def __init__(self, parts=()):
        self.parts = list(parts)

    def __add__(self, other):
        return Code(self.parts + other.parts)

    def memory_size(self):
        size = 0
        for part in self.parts:
            if part[0] == 'label':
                continue
            if part[0] == 'jump':
                size += 2
            else:
                size += part[2]
        return size


def raw(data, memory_size=None):
    data = bytes(data)
    return Code([('raw', data, len(data) if memory_size is None else memory_size)])


def u8(v):
    return raw([v])


def u16(v):
    return raw(struct.pack('>H', v))


def ref(index):
    """An object reference: 4 bytes stored, 8 in memory."""
    return raw(struct.pack('>i', index), 8)


def name(index):
    return raw(struct.pack('>ii', index, 0))


def label(text):
    return Code([('label', text)])


def target(text):
    """The memory offset of a label, as a jump's operand."""
    return Code([('jump', text)])


def seq(*codes):
    out = Code()
    for c in codes:
        out = out + c
    return out


# Expressions and statements.
def local(prop):
    return u8(EX_LOCAL_VARIABLE) + ref(prop)


def instance(prop):
    return u8(EX_INSTANCE_VARIABLE) + ref(prop)


def bool_variable(variable):
    return u8(EX_BOOL_VARIABLE) + variable


def let(lhs, rhs):
    return u8(EX_LET) + lhs + rhs


def let_bool(lhs, rhs):
    return u8(EX_LET_BOOL) + lhs + rhs


def cast(cls, expr):
    return u8(EX_DYNAMIC_CAST) + ref(cls) + expr


def none():
    return u8(EX_NO_OBJECT)


def true():
    return u8(EX_TRUE)


def false():
    return u8(EX_FALSE)


def int_const(v):
    return u8(EX_INT_CONST) + raw(struct.pack('>i', v))


def float_const(v):
    return u8(EX_FLOAT_CONST) + raw(struct.pack('>f', v))


def name_const(index):
    return u8(EX_NAME_CONST) + name(index)


def self_():
    return u8(EX_SELF)


def int_one():
    return u8(EX_INT_ONE)


def nothing():
    return u8(EX_NOTHING)


def empty_parm():
    return u8(EX_EMPTY_PARM_VALUE)


def object_const(obj):
    return u8(0x20) + ref(obj)


def new(cls, outer=None):
    """new(<outer>) class'<cls>' (no name, flags or template)."""
    return u8(EX_NEW) + (outer or nothing()) + nothing() + nothing() + object_const(cls) + nothing()


def context(obj, member, result=0, call_only=False):
    """obj.member; `result` is the property that holds the member's value (0 for none), which the
    engine clears when obj is None, skipping the member's bytecode. For a call on Outer
    (`call_only`), the script compiler's skip covers only the call's token and name."""
    assert all(p[0] == 'raw' for p in member.parts), 'no labels inside a context'
    skip = 1 + 8 if call_only else member.memory_size()
    return u8(EX_CONTEXT) + obj + u16(skip) + ref(result) + u8(0) + member


def call(function_name, *args):
    """A virtual function call by name."""
    return u8(EX_VIRTUAL_FUNCTION) + name(function_name) + seq(*args) + u8(EX_END_FUNCTION_PARMS)


def call_final(function, *args):
    return u8(EX_FINAL_FUNCTION) + ref(function) + seq(*args) + u8(EX_END_FUNCTION_PARMS)


def native(token, *args):
    return u8(token) + seq(*args) + u8(EX_END_FUNCTION_PARMS)


def and_also(a, b):
    """a && b: the second operand is skipped (with the end of the parameters) when a is false."""
    return u8(AND_AND_BOOL_BOOL) + a + u8(EX_SKIP) + u16(b.memory_size() + 1) + b + u8(EX_END_FUNCTION_PARMS)


def struct_member(member, struct_, expr):
    """expr.member, a member of a struct value (`struct_` its struct), here assigned to."""
    return u8(EX_STRUCT_MEMBER) + ref(member) + ref(struct_) + u8(0) + u8(1) + expr


def array_element(index, array):
    return u8(EX_DYN_ARRAY_ELEMENT) + index + array


def array_length(array):
    return u8(EX_DYN_ARRAY_LENGTH) + array


def jump_if_not(to, condition):
    return u8(EX_JUMP_IF_NOT) + target(to) + condition


def jump(to):
    return u8(EX_JUMP) + target(to)


def ret(value=None):
    return u8(EX_RETURN) + (value if value is not None else nothing())


def return_nothing(result):
    """The end of a function that returns a value: returns `result` (its ReturnValue) as it is."""
    return u8(EX_RETURN) + u8(EX_RETURN_NOTHING) + ref(result)


def stop():
    return u8(EX_STOP)


def end():
    return u8(EX_END_OF_SCRIPT)


def assemble(code):
    """(stored bytecode, its size in memory), with each jump's label resolved."""
    labels, offset = {}, 0
    for part in code.parts:
        if part[0] == 'label':
            labels[part[1]] = offset
        elif part[0] == 'jump':
            offset += 2
        else:
            offset += part[2]
    out = bytearray()
    for part in code.parts:
        if part[0] == 'jump':
            out += struct.pack('>H', labels[part[1]])
        elif part[0] == 'raw':
            out += part[1]
    return bytes(out), offset


def assemble_state(code, labels, none):
    """A state's code: `code`, then its label table (each of `labels`, {label: name index}, at its
    offset, then None), aligned to 4 bytes in memory with Nothing tokens, as the compiler writes it.
    Returns (stored bytecode, size in memory, the label table's offset in memory)."""
    while (code.memory_size() + 1) % 4:
        code = code + nothing()
    table_offset = code.memory_size() + 1
    offsets, offset = {}, 0
    for part in code.parts:
        if part[0] == 'label':
            offsets[part[1]] = offset
        elif part[0] == 'jump':
            offset += 2
        else:
            offset += part[2]
    table = u8(EX_LABEL_TABLE)
    for text, index in labels.items():
        table = table + name(index) + raw(struct.pack('>I', offsets[text]))
    table = table + name(none) + raw(struct.pack('>I', 0xFFFF))
    stored, size = assemble(code + table + end())
    return stored, size, table_offset


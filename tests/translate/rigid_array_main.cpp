// RigidArray's formatter is only compiled here, not run: it calls
// get_synthetic_value() on plain members, which is null unless the member's type
// has a synthetic provider of its own (as the Swift types do).
//
// The bytes are checked against the program in
// lldb/test/Shell/ScriptInterpreter/Python/Inputs/FormatterBytecode/RigidArrayLLDBFormatterC.txt,
// disassembled, with two differences: its trailing `return`s are not needed,
// and its `N pick` is spelled `Nu pick` (LLDB's `pick` only accepts a UInt
// index, and `N` assembles to an Integer). The record also has the cascade
// flag, which the translator sets as `type summary add` does.
#include "rigid_array.h"

using namespace lldbfmt;

constexpr byte_buffer expected = record_from_assembly("RigidArray", type_options::cascade,
    "@update: 0u pick \"_storage\" @get_child_with_name call @get_synthetic_value call "
    "0u pick \"_count\" @get_child_with_name call @get_synthetic_value call "
    "@get_value_as_unsigned call 1u "
    "@get_num_children: 2u pick "
    "@get_child_at_index: 1u pick 3u pick @get_child_at_index call");

constexpr bool matches()
{
    if (RigidArraySynthetic.size() != expected.size)
        return false;
    for (size_t i = 0; i < expected.size; ++i) {
        if (RigidArraySynthetic[i] != expected.data[i])
            return false;
    }
    return true;
}
static_assert(matches(), "the translated RigidArray formatter differs from LLDB's own compiled bytecode");

int main() { }

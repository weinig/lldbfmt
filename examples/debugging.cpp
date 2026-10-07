// What lldbfmt/debug.h reports, shown with formatters that are wrong on
// purpose.
//
// Every case is a separate formatter selected with -DDEMO=n, and every one but
// DEMO 0 stops the build with a message. Try one:
//
//   clang++ -std=c++20 -fsyntax-only -Iinclude -DDEMO=1 examples/debugging.cpp
//
// or print them all, with a line on what each shows:
//
//   make debugging-demo
//
// Without -DDEMO the file compiles cleanly (a correct formatter).

#include "lldbfmt/lldbfmt.h"
#include "lldbfmt/script.h"

using namespace lldbfmt;

struct P {
    int a;
    int b;
};

#ifndef DEMO
#define DEMO 0
#endif

#if DEMO == 0
// DEMO 0: nothing is wrong. This builds.
LLDBFMT_ASSEMBLY(Fine, "P", 0, "@summary: 0u pick \"a\" @get_child_with_name call @get_value call return");

#elif DEMO == 1
// DEMO 1: a type mismatch. `1 +` adds an Integer to what `dup` copied, which is
// the object. The verifier says what it expected and what it found, shows the
// program with its blocks indented, and points at the instruction.
LLDBFMT_ASSEMBLY(Mismatch, "P", 0, "@summary: dup 1 + drop \"x\" return");

#elif DEMO == 2
// DEMO 2: UInt and Integer do not mix. The interpreter would stop with "unsupported
// data types". This is found before the program ever reaches LLDB. (The Script
// DSL widens the UInt for you, see DEMO 8.)
LLDBFMT_ASSEMBLY(Mixed, "P", 0, "@summary: 1u 1 + drop \"x\" return");

#elif DEMO == 3
// DEMO 3: `pick` counts from the bottom of the stack, and a summary starts with
// one value, the object. Slot 5 does not exist. (LLDB reports
// "index out of bounds(opcode=pick)".)
LLDBFMT_ASSEMBLY(OutOfRange, "P", 0, "@summary: 5u pick @get_value call return");

#elif DEMO == 4
// DEMO 4: a program with a conditional, to show the layout: a block per group of
// lines, `} {` between the two arms. The mistake is that one arm leaves a
// String and the other nothing, so after the `ifelse` the stack has a different
// number of values depending on the path.
LLDBFMT_ASSEMBLY(Unbalanced, "P", 0,
    "@summary: 0u pick \"a\" @get_child_with_name call @get_value_as_unsigned call "
    "{ \"big\" } { } ifelse return");

#elif DEMO == 5
// DEMO 5: `@fmt` prints a bogus pointer for a UInt or Int argument in today's LLDB
// (it formats the argument's address, not its value). The verifier knows, and
// says what to do instead. The Script DSL's format() avoids this by reading the
// member as a String.
LLDBFMT_ASSEMBLY(BogusFormat, "P", 0,
    "@summary: 0u pick \"a\" @get_child_with_name call @get_value_as_unsigned call "
    "\"{0}\" @fmt call swap drop return");

#elif DEMO == 6
// DEMO 6: a summary has to return a String. This one returns the number.
LLDBFMT_ASSEMBLY(WrongResult, "P", 0,
    "@summary: 0u pick \"a\" @get_child_with_name call @get_value_as_unsigned call return");

#elif DEMO == 7
// DEMO 7: a selector in the spec that LLDB's interpreter has not implemented
// (here @read_memory_byte). It would fail at run time with "selector not
// implemented".
LLDBFMT_ASSEMBLY(NotImplemented, "P", 0, "@summary: 0u @read_memory_byte call drop \"x\" return");

#elif DEMO == 8
// DEMO 8: the Script DSL checks types as you write them: an Int (get_value_as_signed)
// and a UInt (get_value_as_unsigned) cannot be added, because the interpreter
// would refuse. There are two messages. The DSL's own comes from the C++
// compiler, at the line that wrote the `+`. Then the verifier's shows the
// bytecode the DSL made anyway, with the `+` marked.
LLDBFMT_SCRIPT_SUMMARY(Added, "P", [](auto valobj) {
    return valobj.get_child_member_with_name("a").get_value_as_signed() + valobj.get_child_member_with_name("b").get_value_as_unsigned();
});

#elif DEMO == 9
// DEMO 9: LLDB drops the rest of a program when a conditional runs inside another
// conditional's block: resuming the outer block overwrites the continuation
// below it with an empty one. The code after the outer `ifelse` builds
// "before inner after", but it would never run, with no error, and LLDB would
// show just "inner". The verifier marks the inner conditional, the one that
// triggers it. (A nested conditional is fine when nothing follows the outer one.)
LLDBFMT_ASSEMBLY(Nested, "P", 0,
    "@summary: \"before\" 1u { 1u { \"inner\" } { \"other\" } ifelse } { \"no\" } ifelse "
    "\"after\" \"{0} {1} {2}\" @fmt call swap drop swap drop swap drop return");

#elif DEMO == 10
// DEMO 10: printing a record you wrote, to see what you got, even when it is
// right. LLDBFMT_DUMP(Name) turns the record into a compile-time message,
// with every program disassembled and indented.
LLDBFMT_SCRIPT_SUMMARY(Described, "P", [](auto valobj) {
    return script::if_(valobj.get_child_member_with_name("a").get_value_as_unsigned() == 0,
        "zero",
        script::format("a is %s", valobj.get_child_member_with_name("a").get_value_as_signed()));
});
LLDBFMT_DUMP(Described);

#elif DEMO == 11
// DEMO 11: trying out assembly before using it. LLDBFMT_PRINT shows any
// program `Bytes`, here what the assembler makes of a few lines, so you can check
// the encoding (`0u pick` is `0x20 0x00 0x03`: lit_uint, 0, pick).
LLDBFMT_PRINT(assemble("0u pick \"a\" @get_child_with_name call 1 { dup } if"));

#elif DEMO == 12
// DEMO 12: print every record a file defines, without touching its source:
// compile with -DLLDBFMT_DUMP_ALL=1 (the Makefile does for this case).
LLDBFMT_ASSEMBLY(First, "P", 0, "@summary: 0u pick \"a\" @get_child_with_name call @get_value call return");
LLDBFMT_SCRIPT_SUMMARY(Second, "P", [](auto valobj) {
    return script::format("(%s, %s)", valobj.get_child_member_with_name("a").get_value_as_signed(), valobj.get_child_member_with_name("b").get_value_as_signed());
});

#endif

int main()
{
}

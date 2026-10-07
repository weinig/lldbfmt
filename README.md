# lldbfmt

Header-only C++20 library that builds [LLDB formatter bytecode](https://lldb.llvm.org/resources/formatterbytecode.html)
during constant evaluation and emits it into the binary's formatter section
in one macro.

There are three layers, each built on the one before:

1. **Low-level** (`lldbfmt/lldbfmt.h`): a stack-oriented DSL and assembler that
   produce and emit the bytecode.
2. **Script** (`lldbfmt/script.h`): a Python-shaped DSL. You write the same
   `SBValue` calls and operators a Python data formatter uses.
3. **Reflection** (`lldbfmt/reflect.h`, C++26): annotate a type and the formatter
   is derived from it, with no formatter code at all.

`lldbfmt/debug.h` (verifier, disassembler, compile-time printing) works on the
low-level layer's bytecode, so it serves all three.

#!/usr/bin/env python3
# Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Checks that the verifier (include/lldbfmt/debug.h) rejects what LLDB's
interpreter would reject, with a useful message, and accepts what it runs.

Each case is a snippet compiled on its own; `fails` lists substrings the
compiler output must contain, and an empty list means it must compile cleanly.
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PRELUDE = """
#include "lldbfmt/lldbfmt.h"
#include "lldbfmt/script.h"
using namespace lldbfmt;
struct P { int a; };
"""

CASES = [
    # name, snippet, substrings that must appear (empty: must compile)
    ("a correct summary", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: 0u pick \\"a\\" @get_child_with_name call @get_value call return");', []),
    ("object + integer", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: dup 1 + return");',
        ["@summary does not verify", "type mismatch", "expected UInt or Int or Integer, found Object", "^ here"]),
    ("UInt + Integer", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: 1u 1 + drop \\"x\\" return");',
        ["operands have different types", "left is UInt, right is Integer"]),
    ("pick with an Integer index", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: 0 pick drop \\"x\\" return");',
        ["type mismatch", "expected UInt, found Integer"]),
    ("pick of a slot that is not there", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: 5u pick drop \\"x\\" return");',
        ["`pick` of a slot that does not exist"]),
    ("popping an empty stack", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: drop drop \\"x\\" return");',
        ["pops a value that is not on the stack"]),
    ("a selector the interpreter lacks", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: dup @read_memory_byte call drop \\"x\\" return");',
        ["does not implement this selector"]),
    ("a call without a selector", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: dup call return");',
        ["type mismatch", "expected Selector, found Object"]),
    ("a summary that returns an integer", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: 1 return");',
        ["returns the wrong type", "expected String, found Integer"]),
    ("a summary that returns nothing", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: drop return");',
        ["no value to return"]),
    ("fmt of a UInt", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: 1u \\"{0}\\" @fmt call swap drop return");',
        ["bogus pointer for a UInt or Int argument"]),
    ("fmt of a string", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: \\"x\\" \\"{0}\\" @fmt call swap drop return");', []),
    ("fmt with a computed format", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: dup @get_value call @fmt call return");',
        ["@fmt needs a string literal"]),
    ("if on a string", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: \\"x\\" { } if \\"y\\" return");',
        ["type mismatch", "expected UInt or Integer, found String"]),
    ("if without a block", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: 1u if \\"y\\" return");',
        ["without the blocks"]),
    ("paths that leave different depths", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: 1u { 1u } { } ifelse return");',
        ["different numbers of values"]),
    ("a conditional nested before more code", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: \\"b\\" 1u { 1u { \\"i\\" } { \\"o\\" } ifelse } { \\"n\\" } ifelse \\"a\\" \\"{0} {1} {2}\\" @fmt call return");',
        ["loses the code after the outer one", "^ here"]),
    ("a nested conditional that is last", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: 1u { 1u { \\"i\\" } { \\"o\\" } ifelse } { \\"n\\" } ifelse");', []),
    ("a nested conditional in a block that returns", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: 1u { 1u { \\"i\\" } { \\"o\\" } ifelse return } if \\"x\\" return");', []),
    ("the Script DSL: a wrong operand mix", 'LLDBFMT_SCRIPT_SUMMARY(A, "P", [](auto v) { return v.get_value_as_signed() + v.get_value_as_unsigned(); });',
        ["no type in common"]),
    ("the Script DSL: a nested conditional before more state",
        'LLDBFMT_SCRIPT_SYNTHETIC(A, "P", update([](auto v) { return state(if_(v.get_num_children() == 1, 1, v.get_num_children() == 2, 2, 3)); }));',
        ["@update does not verify", "loses the code after the outer one"]),
    ("the allow switch", 'LLDBFMT_ASSEMBLY(A, "P", 0, "@summary: \\"b\\" 1u { 1u { \\"i\\" } { \\"o\\" } ifelse } { \\"n\\" } ifelse \\"a\\" \\"{0} {1} {2}\\" @fmt call swap drop swap drop swap drop return");',
        ["loses the code after the outer one"]),
]

DUMP = ("LLDBFMT_ASSEMBLY(A, \"P\", 0, \"@summary: 2u 1u = { \\\"a\\\" } { 1u { \\\"b\\\" } { \\\"c\\\" } ifelse } ifelse\");\n"
        "LLDBFMT_DUMP(A);")


def compile_snippet(snippet, extra=()):
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "case.cpp")
        with open(path, "w") as f:
            f.write(PRELUDE + snippet + "\nint main() {}\n")
        return subprocess.run(
            [os.environ.get("CXX", "clang++"), "-std=c++20", "-fno-exceptions", "-fsyntax-only",
             "-I", os.path.join(ROOT, "include"), *extra, path],
            capture_output=True, text=True,
        )  # fmt: skip


# examples/debugging.cpp: every case must stop the build with its message.
DEMOS = {
    0: [],
    1: ["type mismatch", "expected UInt or Int or Integer, found Object", "^ here"],
    2: ["operands have different types", "left is UInt, right is Integer"],
    3: ["`pick` of a slot that does not exist"],
    4: ["different numbers of values", "} {"],
    5: ["bogus pointer for a UInt or Int argument"],
    6: ["returns the wrong type for this signature", "expected String, found UInt"],
    7: ["does not implement this selector"],
    8: ["no type in common", "does not verify"],
    9: ["loses the code after the outer one", "^ here"],
    10: ["lldbfmt record \"P\"", "@summary", "} {"],
    11: ["0u pick \"a\" @get_child_with_name call 1 {"],
    12: ["lldbfmt record \"P\" (flags 0", "lldbfmt record \"P\" (flags 1"],
}


def check_demos():
    failures = 0
    for number, expected in DEMOS.items():
        extra = ["-DLLDBFMT_DUMP_ALL=1"] if number == 12 else []
        result = subprocess.run(
            [os.environ.get("CXX", "clang++"), "-std=c++20", "-fno-exceptions", "-fsyntax-only", "-I", os.path.join(ROOT, "include"),
             f"-DDEMO={number}", *extra, os.path.join(ROOT, "examples", "debugging.cpp")],
            capture_output=True, text=True,
        )  # fmt: skip
        ok = (result.returncode == 0) if not expected else (result.returncode != 0 and all(e in result.stderr for e in expected))
        print(("ok    " if ok else "FAIL  ") + f"examples/debugging.cpp DEMO {number}")
        if not ok:
            failures += 1
            print(result.stderr[:1500])
    return failures


def main():
    failures = check_demos()
    for name, snippet, expected in CASES:
        result = compile_snippet(snippet)
        output = result.stderr
        if not expected:
            ok = result.returncode == 0
        else:
            ok = result.returncode != 0 and all(e in output for e in expected)
        print(("ok    " if ok else "FAIL  ") + name)
        if not ok:
            failures += 1
            print(output[:1500])

    # LLDBFMT_DUMP prints a record.
    result = compile_snippet(DUMP)
    expected = ['lldbfmt record "P"', "@summary", "2u 1u = {", '"a"', "} {", "ifelse"]
    ok = all(e in result.stderr for e in expected)
    print(("ok    " if ok else "FAIL  ") + "LLDBFMT_DUMP prints the disassembly")
    if not ok:
        failures += 1
        print(result.stderr[:1500])

    # -DLLDBFMT_ALLOW_NESTED_CONDITIONALS=1 accepts what LLDB loses.
    result = compile_snippet(CASES[-1][1], ["-DLLDBFMT_ALLOW_NESTED_CONDITIONALS=1", "-DLLDBFMT_FMT_CONSUMES_ARGUMENTS=0"])
    ok = "loses the code" not in result.stderr
    print(("ok    " if ok else "FAIL  ") + "the nested-conditional check can be turned off")
    failures += 0 if ok else 1

    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

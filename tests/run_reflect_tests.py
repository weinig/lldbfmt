#!/usr/bin/env python3
# Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for include/lldbfmt/reflect.h. Needs a compiler with C++26
reflection (GCC 16: --cxx /opt/homebrew/bin/g++-16) and, for the run-time part,
a recent LLDB.

    tests/run_reflect_tests.py --cxx g++-16 [--lldb PATH] [--no-run]
"""

import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PRELUDE = """
#include "lldbfmt/reflect.h"
namespace lldb = lldbfmt::reflect;
"""

# Compile errors: a snippet, and what the compiler output must say.
ERRORS = [
    ("a placeholder naming no member",
     'struct [[=lldb::summary("{z}")]] P { int x; int y; };\nLLDBFMT_REFLECT(P);',
     ["summary of P: no member 'z' (members: x, y)"]),
    ("a placeholder naming a hidden member",
     'struct [[=lldb::summary("{secret}")]] P { int x; [[=lldb::hidden {}]] int secret; };\nLLDBFMT_REFLECT(P);',
     ["'secret' is hidden"]),
    ("a renamed member used by its old name",
     'struct [[=lldb::summary("{x}")]] P { [[=lldb::rename("width")]] int x; };\nLLDBFMT_REFLECT(P);',
     ["'x' is shown as 'width'; use that name"]),
    ("a member whose type has no summary",
     'struct Inner { int a; int b; };\nstruct [[=lldb::summary("{inner}")]] P { Inner inner; };\nLLDBFMT_REFLECT(P);',
     ["'{inner}' is a Inner, which has no summary", "{inner.a}"]),
    ("an unterminated placeholder",
     'struct [[=lldb::summary("{x")]] P { int x; };\nLLDBFMT_REFLECT(P);',
     ["'{' without a '}'"]),
    ("an array on something that is not a pointer",
     'struct P { [[=lldb::array("n")]] int data; unsigned n; };\nLLDBFMT_REFLECT(P);',
     ["belongs on a pointer, but 'data' is not one"]),
    ("an array whose count does not exist",
     'struct P { [[=lldb::array("n")]] int* data; unsigned count; };\nLLDBFMT_REFLECT(P);',
     ["array(\"n\") names no member (members: count)"]),
    ("two arrays",
     'struct P { [[=lldb::array("n")]] int* a; [[=lldb::array("n")]] int* b; unsigned n; };\nLLDBFMT_REFLECT(P);',
     ["only one member can be an array"]),
    ("a correct type", 'struct [[=lldb::summary("{x}")]] P { int x; };\nLLDBFMT_REFLECT(P);', []),
    ("a dump of a generated record",
     'struct [[=lldb::summary("({x}, {y})")]] P { int x; int y; };\nLLDBFMT_REFLECT_NAMED(P, named);\nLLDBFMT_PRINT_RECORD(named_summary_record);',
     ['lldbfmt record "P"', '"({0}, {1})"', "@fmt call"]),
]


def compile_snippet(cxx, snippet):
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "case.cpp")
        with open(path, "w") as f:
            f.write(PRELUDE + snippet + "\nint main() {}\n")
        return subprocess.run(
            [cxx, "-std=c++26", "-freflection", "-Wno-attributes", "-fsyntax-only", "-I", os.path.join(ROOT, "include"), path],
            capture_output=True, text=True,
        )  # fmt: skip


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=os.environ.get("CXX_REFLECT", "/opt/homebrew/bin/g++-16"))
    parser.add_argument("--lldb", default=os.environ.get("LLDB", "lldb"))
    parser.add_argument("--no-run", action="store_true")
    args = parser.parse_args()

    failures = 0
    for name, snippet, expected in ERRORS:
        result = compile_snippet(args.cxx, snippet)
        if not expected:
            ok = result.returncode == 0
        elif name.startswith("a dump"):
            ok = all(e in result.stderr for e in expected)
        else:
            ok = result.returncode != 0 and all(e in result.stderr for e in expected)
        print(("ok    " if ok else "FAIL  ") + name)
        if not ok:
            failures += 1
            print(result.stderr[:2000])

    # examples/debugging_reflection.cpp: each case stops with its message.
    demos = {
        0: [],
        1: ["summary of Point: no member 'z' (members: x, y)"],
        2: ["'password' is hidden"],
        3: ["'name' is shown as 'user'; use that name"],
        4: ["'{where}' is a Point, which has no summary", "{where.x}"],
        5: ["belongs on a pointer, but 'data' is not one"],
        6: ['lldbfmt record "Point"', '"({0}, {1})"'],
        7: ["@get_child_at_index", "@get_num_children", "if"],
    }
    for number, expected in demos.items():
        result = subprocess.run(
            [args.cxx, "-std=c++26", "-freflection", "-Wno-attributes", "-fsyntax-only", "-I", os.path.join(ROOT, "include"),
             f"-DDEMO={number}", os.path.join(ROOT, "examples", "debugging_reflection.cpp")],
            capture_output=True, text=True,
        )  # fmt: skip
        ok = (result.returncode == 0) if not expected else (result.returncode != 0 and all(e in result.stderr for e in expected))
        print(("ok    " if ok else "FAIL  ") + f"examples/debugging_reflection.cpp DEMO {number}")
        if not ok:
            failures += 1
            print(result.stderr[:1500])

    # Build the example and check what LLDB prints.
    build = os.path.join(ROOT, "build")
    os.makedirs(build, exist_ok=True)
    program = os.path.join(build, "reflection")
    result = subprocess.run(
        [args.cxx, "-std=c++26", "-freflection", "-g", "-Wall", "-Wextra", "-Wno-attributes", "-I", os.path.join(ROOT, "include"),
         os.path.join(ROOT, "examples", "reflection.cpp"), "-o", program],
        capture_output=True, text=True,
    )  # fmt: skip
    ok = result.returncode == 0 and "warning" not in result.stderr
    print(("ok    " if ok else "FAIL  ") + "examples/reflection.cpp builds without warnings")
    if not ok:
        failures += 1
        print(result.stderr[:2000])
    elif not args.no_run:
        output = subprocess.run(
            [args.lldb, "-b", "-o", "b stop", "-o", "run", "-o", "up", "-o", "frame variable point rect account buffer plain job marker", program],
            capture_output=True, text=True,
        ).stdout  # fmt: skip
        expectations = [
            "(Geometry::Point) point = (3, 4) {",
            "(Geometry::Rect) rect = (1, 2) + (30, 40) {",
            '(Account) account = "ada" {',
            'user = 0x',
            "logins = 7",
            "(IntBuffer) buffer = 3 items {",
            "(Buffer<int>) plain = 2 items {",
            "[1] = 20",
            "(Job) job = Slow 1 {",
            '(Marker) marker = "pin" at (5, 6) {',
            "Geometry::Point = (5, 6) {",
        ]
        missing = [e for e in expectations if e not in output]
        shown = [l for l in output.splitlines() if l.startswith("(Account)") or l.strip().startswith(("user =", "password", "name"))]
        unwanted = [l for l in shown if "password" in l or "hunter2" in l or " name =" in l]
        unwanted += [l for l in output.splitlines() if l.strip().startswith("internalId") or l.strip().endswith("= 99")]
        ok = not missing and not unwanted
        print(("ok    " if ok else "FAIL  ") + "LLDB shows what the annotations say")
        if not ok:
            failures += 1
            print(f"missing {missing}, unwanted {unwanted}\n{output}")

    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Translate Python formatters to C++, build them, and run them in LLDB.

    tests/run_translate_tests.py [--lldb PATH] [--no-run]

Each case translates a Python formatter with `formatter_bytecode.py --compile
--format cpp`, compiles a small program that includes the result, and (unless
--no-run) checks what `frame variable` prints in LLDB. The embedded formatter
bytecode needs a recent LLDB; the one in Xcode may be too old.
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "translate")

# python is the input formatter, header the name of the generated header, main
# the program (in tests/translate), expect the substrings that must appear in
# the output of the commands, and type_name is for inputs without a registration.
CASES = [
    {
        "name": "shared_ptr (lldb/examples/summaries/sp_cp.py)",
        "python": "sp_cp.py",
        "header": "sp_cp.h",
        "main": "sp_cp_main.cpp",
        "commands": ["frame variable pointer"],
        # The provider reports num_children = 1, so only the first child shows.
        "expect": ["(IntSP) pointer = use = 2 {", "_M_ptr = "],
    },
    {
        "name": "MyOptional (lldb/test/.../FormatterBytecode/formatter.py)",
        "python": "my_optional.py",
        "header": "my_optional.h",
        "main": "my_optional_main.cpp",
        "commands": ["frame variable x y", "frame variable *y"],
        "expect": [
            "(MyOptional<int>) x = None {",
            "(MyOptional<int>) y = 42 {",
            "(int) *y = 42",
        ],
    },
    {
        "name": "std::vector (lldb/examples/synthetic/gnu_libstdcpp.py)",
        "python": "std_vector.py",
        "header": "std_vector.h",
        "main": "std_vector_main.cpp",
        "commands": ["frame variable numbers empty", "frame variable numbers[2]"],
        "expect": [
            "[0] = 3",
            "[3] = 1",
            "(std::vector<int>) numbers = size=4 {",
            "(std::vector<int>) empty = size=0 {}",
            "(int) numbers[2] = 4",
        ],
    },
    {
        "name": "Account (lldb/test/API/.../bytecode-synthetic), written in the DSL",
        "python": None,
        "main": "account_main.cpp",
        "commands": ["log enable lldb formatters", "frame variable acc"],
        "expect": [
            "(Account) acc = (username = ",
            "Bytecode formatter can reuse @update: true (type: `Account`, name: `acc`)",
        ],
        "reject": ["password"],
    },
    {
        "name": "RigidArray (lldb/test/Shell/.../python-bytecode.test)",
        "python": "rigid_array.py",
        "header": "rigid_array.h",
        "main": "rigid_array_main.cpp",
        "type_name": "RigidArray",
        "commands": None,  # Compiled only. See the comment in the program.
        "expect": [],
    },
]


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lldb", default=os.environ.get("LLDB", "lldb"))
    parser.add_argument("--no-run", action="store_true", help="translate and compile only")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    args = parser.parse_args()

    os.makedirs(BUILD, exist_ok=True)
    failures = 0
    for case in CASES:
        print(f"== {case['name']}")
        if case["python"] is None:
            translate = None
        else:
            header = os.path.join(BUILD, case["header"])
        translate = None if case["python"] is None else [
            sys.executable, os.path.join(ROOT, "tools", "formatter_bytecode.py"),
            "--compile", os.path.join(ROOT, "tests", "translate", case["python"]),
            "--format", "cpp", "--output", header,
        ]  # fmt: skip
        if translate is not None:
            if "type_name" in case:
                translate += ["--type-name", case["type_name"]]
            result = run(translate)
            if result.returncode != 0:
                print(f"  FAIL: translation\n{result.stderr}")
                failures += 1
                continue

        program = os.path.join(BUILD, os.path.splitext(case["main"])[0])
        compile_ = [
            args.cxx, "-std=c++20", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-I", os.path.join(ROOT, "include"), "-I", BUILD,
            "-I", os.path.join(ROOT, "tests", "translate"),
            os.path.join(ROOT, "tests", "translate", case["main"]), "-o", program,
        ]  # fmt: skip
        result = run(compile_)
        if result.returncode != 0 or "warning" in result.stderr:
            print(f"  FAIL: compile\n{result.stderr}")
            failures += 1
            continue
        if args.no_run or case["commands"] is None:
            print("  ok (compiled)")
            continue

        lldb = [args.lldb, "-b", "-o", "b stop", "-o", "run", "-o", "up"]
        for command in case["commands"]:
            lldb += ["-o", command]
        output = run(lldb + [program]).stdout
        missing = [e for e in case["expect"] if e not in output]
        # `reject` applies to the lines that show the variable, not to log lines.
        shown = [l for l in output.splitlines() if l.startswith("(") and " = " in l]
        unwanted = [r for r in case.get("reject", []) if any(r in l for l in shown)]
        if missing or unwanted:
            print(f"  FAIL: missing {missing}, unexpected {unwanted}\n{output}")
            failures += 1
        else:
            print("  ok")
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

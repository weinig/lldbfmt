#!/usr/bin/env python3
# Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Builds lldbfmt as a module and uses it, with each compiler given.

    tests/run_module_tests.py [--clang PATH] [--gcc PATH] [--lldb PATH]

Apple's clang does not support C++20 named modules; use a clang from LLVM (14 or
later, a newer one is better) and/or GCC 14 or later. Each compiler builds
modules/lldbfmt.cppm, compiles examples/module_example.cpp against it, and (with
--lldb) checks what LLDB shows. GCC 16 with reflection also builds the module
with the reflection layer and compiles examples/module_reflection_example.cpp.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "modules")
INCLUDE = os.path.join(ROOT, "include")


def run(command, cwd=None):
    return subprocess.run(command, capture_output=True, text=True, cwd=cwd)


def sdk_flags():
    result = run(["xcrun", "--show-sdk-path"]) if sys.platform == "darwin" else None
    return ["-isysroot", result.stdout.strip()] if result and result.returncode == 0 else []


def lldb_output(lldb, program, variables):
    return run([lldb, "-b", "-o", "b stop", "-o", "run", "-o", "up", "-o", f"frame variable {variables}", program]).stdout


def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name)
    if not ok and detail:
        print(detail[:2000])
    return 0 if ok else 1


def clang_case(clang, lldb):
    failures = 0
    directory = os.path.join(BUILD, "clang")
    os.makedirs(directory, exist_ok=True)
    pcm = os.path.join(directory, "lldbfmt.pcm")
    flags = ["-std=c++20", "-I", INCLUDE, *sdk_flags()]
    result = run([clang, *flags, "--precompile", os.path.join(ROOT, "modules", "lldbfmt.cppm"), "-o", pcm])
    failures += check("clang: build the module", result.returncode == 0 and os.path.exists(pcm), result.stderr)
    if failures:
        return failures
    program = os.path.join(directory, "module_example")
    result = run([clang, *flags, "-g", f"-fmodule-file=lldbfmt={pcm}", os.path.join(ROOT, "examples", "module_example.cpp"), pcm, "-o", program])
    ok = result.returncode == 0 and "error" not in result.stderr
    failures += check("clang: compile a program that imports it", ok, result.stderr)
    if ok and lldb:
        output = lldb_output(lldb, program, "p c")
        failures += check("clang: LLDB shows the formatters", "(point) p = (3, 4) {" in output and "(counter) c = 7 {" in output, output)
    return failures


def gcc_case(gcc, lldb):
    failures = 0
    directory = os.path.join(BUILD, "gcc")
    os.makedirs(directory, exist_ok=True)
    flags = ["-std=c++26", "-fmodules", "-g", "-Wno-attributes", "-I", INCLUDE]
    module_object = os.path.join(directory, "lldbfmt.o")
    cache = os.path.join(directory, "gcm.cache")
    run(["rm", "-rf", cache])
    # With reflection, the module includes the reflection layer.
    probe = run([gcc, "-std=c++26", "-freflection", "-dM", "-E", "-x", "c++", "/dev/null"])
    reflect = probe.returncode == 0 and "__cpp_impl_reflection" in probe.stdout
    flags_reflect = [*flags, "-freflection"] if reflect else flags
    result = run([gcc, *flags_reflect, "-c", os.path.join(ROOT, "modules", "lldbfmt.cppm"), "-o", module_object], cwd=directory)
    failures += check("gcc: build the module" + (" (with reflection)" if reflect else ""), result.returncode == 0, result.stderr)
    if failures:
        return failures
    program = os.path.join(directory, "module_example")
    result = run([gcc, *flags_reflect, os.path.join(ROOT, "examples", "module_example.cpp"), module_object, "-o", program], cwd=directory)
    ok = result.returncode == 0
    failures += check("gcc: compile a program that imports it", ok, result.stderr)
    if ok and lldb:
        output = lldb_output(lldb, program, "p c")
        failures += check("gcc: LLDB shows the formatters", "(point) p = (3, 4) {" in output and "(counter) c = 7 {" in output, output)
    if reflect:
        program = os.path.join(directory, "module_reflection_example")
        result = run([gcc, *flags_reflect, os.path.join(ROOT, "examples", "module_reflection_example.cpp"), module_object, "-o", program], cwd=directory)
        ok = result.returncode == 0
        failures += check("gcc: compile a program that imports the reflection layer", ok, result.stderr)
        if ok and lldb:
            output = lldb_output(lldb, program, "p a")
            shown = "(point) p = (3, 4) {" in output and '(account) a = (name = "ada")' in output
            hidden = not any("hunter2" in l or l.strip().startswith("password") for l in output.splitlines() if re.match(r"^(\(|  \S)", l))
            failures += check("gcc: LLDB shows the annotated formatters", shown and hidden, output)
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", default=os.environ.get("MODULE_CLANG"), help="a clang that supports named modules")
    parser.add_argument("--gcc", default=os.environ.get("MODULE_GCC", "/opt/homebrew/bin/g++-16"))
    parser.add_argument("--lldb", default=os.environ.get("LLDB"))
    args = parser.parse_args()

    failures = 0
    ran = False
    if args.clang:
        ran = True
        failures += clang_case(args.clang, args.lldb)
    if args.gcc and os.path.exists(args.gcc):
        ran = True
        failures += gcc_case(args.gcc, args.lldb)
    if not ran:
        print("no compiler: pass --clang and/or --gcc")
        return 1
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Print what the compiler says for each case of the deliberately failing
examples (examples/debugging.cpp and examples/debugging_reflection.cpp).

    tools/debugging_demo.py [--cxx clang++] [--reflect-cxx g++-16] [CASE...]

CASE is `debugging:N` or `reflection:N`; with none, all of them are shown. The
explanation above each case is its comment in the source.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FILES = {
    "debugging": ("examples/debugging.cpp", ["-std=c++20", "-fno-exceptions"], "cxx"),
    "reflection": ("examples/debugging_reflection.cpp", ["-std=c++26", "-freflection", "-Wno-attributes"], "reflect"),
}


def cases(path):
    """(number, explanation) for each `#if DEMO == n` in the file."""
    lines = open(os.path.join(ROOT, path)).read().splitlines()
    result = []
    for i, line in enumerate(lines):
        match = re.match(r"#(?:el)?if DEMO == (\d+)", line)
        if not match:
            continue
        explanation = []
        for comment in lines[i + 1 :]:
            if not comment.startswith("//"):
                break
            explanation.append(comment[3:] if comment.startswith("// ") else "")
        result.append((int(match.group(1)), explanation))
    return result


def message(output):
    """The diagnostics without the macro-expansion noise."""
    kept = []
    skip = False
    for line in output.splitlines():
        if "note: expanded from macro" in line or "in definition of macro" in line or "in expansion of macro" in line:
            skip = True
            continue
        if re.match(r"\s+\d+ \|", line) or re.match(r"\s+\|", line) or "<scratch space>" in line:
            continue
        if re.match(r"^(In file included|\s+from )", line):
            continue
        if "note: in instantiation of" in line or "requested here" in line or "in 'constexpr' expansion" in line:
            continue
        if line.startswith(("note:", "In ")) and skip:
            continue
        if re.search(r"^\S+:\d+:\d+: (note|warning): ", line) and skip:
            continue
        skip = False
        kept.append(line)
    text = "\n".join(kept).strip()
    return re.sub(r"\n+\d+ errors? generated\.$|\n+1 error generated\.$", "", text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--reflect-cxx", default=os.environ.get("REFLECT_CXX", "/opt/homebrew/bin/g++-16"))
    parser.add_argument("selected", nargs="*")
    args = parser.parse_args()

    for name, (path, flags, which) in FILES.items():
        compiler = args.cxx if which == "cxx" else args.reflect_cxx
        for number, explanation in cases(path):
            if args.selected and f"{name}:{number}" not in args.selected:
                continue
            extra = ["-DLLDBFMT_DUMP_ALL=1"] if (name, number) == ("debugging", 12) else []
            result = subprocess.run(
                [compiler, *flags, "-fsyntax-only", "-I", os.path.join(ROOT, "include"), f"-DDEMO={number}", *extra, os.path.join(ROOT, path)],
                capture_output=True, text=True,
            )  # fmt: skip
            print("=" * 78)
            print(f"{name} DEMO {number}")
            print("\n".join("  " + line if line else "" for line in explanation))
            print("-" * 78)
            print(message(result.stderr) if result.stderr else "(compiles cleanly)")
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

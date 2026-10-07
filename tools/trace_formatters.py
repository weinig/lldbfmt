#!/usr/bin/env python3
# Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Run LLDB on a program and show what its embedded formatters did, step by step.

    tools/trace_formatters.py [--lldb PATH] [--break stop] [--steps] PROGRAM COMMAND...

Starts PROGRAM under LLDB, stops at the breakpoint (a function or `file:line`),
goes up one frame, turns on LLDB's verbose formatter log, runs each COMMAND
(for example `frame variable numbers`), and prints a readable trace for every
formatter program that ran: which program, how many steps, the data stack at the
end, and any error LLDB reported. With --steps every instruction is listed.

The log shows two things that are otherwise hard to see: a program that ends
early (fewer steps than expected, no error), and the exact value on the stack
where a type check failed.
"""

import argparse
import re
import subprocess
import sys

STEP = re.compile(r"\[eval (@\w+)\] opcode=(.*?), control=(\d+), data=\[ ?(.*?) ?\]\s*$")
ERROR = re.compile(r"^ (@\w+ failed: .*|Bytecode formatter .*)$")


def simplify(data: str) -> str:
    data = re.sub(r"object\(0x[0-9a-f]+\)", "obj", data)
    return data.replace("object()", "null") or "(empty)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lldb", default="lldb")
    parser.add_argument("--break", dest="breakpoint", default="stop", help="where to stop (default: stop)")
    parser.add_argument("--steps", action="store_true", help="list every instruction")
    parser.add_argument("program")
    parser.add_argument("commands", nargs="+")
    args = parser.parse_args()

    command = [args.lldb, "-b", "-o", f"b {args.breakpoint}", "-o", "run", "-o", "up",
               "-o", "log enable -v lldb formatters"]  # fmt: skip
    for c in args.commands:
        command += ["-o", c]
    output = subprocess.run(command + [args.program], capture_output=True, text=True).stdout

    runs: list[dict] = []
    current = None
    for line in output.splitlines():
        step = STEP.search(line)
        if step:
            signature, op, control, data = step.groups()
            if current is None or current["signature"] != signature:
                current = {"signature": signature, "steps": [], "notes": []}
                runs.append(current)
            current["steps"].append((op, int(control), simplify(data)))
        elif ERROR.match(line) and "FormatManager" not in line:
            if current is None:
                current = {"signature": "(no program ran)", "steps": [], "notes": []}
                runs.append(current)
            current["notes"].append(line.strip())

    for run in runs:
        steps = run["steps"]
        print(f"{run['signature']}: {len(steps)} steps")
        if args.steps:
            for index, (op, control, data) in enumerate(steps):
                print(f"  {index:4}  {op:<22} control={control}  [ {data} ]")
        elif steps:
            op, control, data = steps[-1]
            print(f"  last instruction: {op} (control stack {control}); data before it: [ {data} ]")
        for note in run["notes"]:
            print(f"  ! {note}")
    printed = [l for l in output.splitlines() if l.startswith("(") and " = " in l]
    if printed:
        print("\nLLDB printed:")
        for l in printed:
            print("  " + l)
    return 0


if __name__ == "__main__":
    sys.exit(main())

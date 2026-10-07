// Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The library used as a module. Macros cannot be exported by a module, so they
// come from macros.h, which declares nothing.
//
//   make module-test MODULE_CXX=/path/to/clang++

import lldbfmt;

#include "lldbfmt/macros.h"

struct point {
    int x;
    int y;
};

// The Script DSL: the same names, from the module.
LLDBFMT_SCRIPT_SUMMARY(point_summary, "point", [](auto valobj) {
    return lldbfmt::script::format("(%s, %s)",
        valobj.get_child_member_with_name("x").get_value_as_signed(),
        valobj.get_child_member_with_name("y").get_value_as_signed());
});

struct counter {
    int count;
};

// Assembly, checked by the verifier like everything else.
LLDBFMT_ASSEMBLY(counter_summary, "counter", lldbfmt::type_options::cascade,
    "@summary: 0u pick \"count\" @get_child_with_name call @get_value call return");

void stop()
{
}

int main()
{
    point p { 3, 4 };
    counter c { 7 };
    stop();
    return p.x + c.count;
}

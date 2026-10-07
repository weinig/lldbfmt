// Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Formatters from annotations, with the library imported as a module. The
// module must have been built with a compiler with C++26 reflection, which
// then includes the reflection layer (see modules/lldbfmt.cppm).
//
//   make module-test

// GCC does not make the standard library names that the module's templates use
// (here from <meta>) reachable in a file that only imports the module, so
// include <meta> first. Other compilers do not need it.
#include <meta>

import lldbfmt;

#include "lldbfmt/macros.h"

namespace lldb = lldbfmt::reflect;

struct [[=lldb::summary("({x}, {y})")]] point {
    int x;
    int y;
};

struct account {
    const char* name;
    [[=lldb::hidden {}]] const char* password;
};

LLDBFMT_REFLECT(point);
LLDBFMT_REFLECT(account);

void stop()
{
}

int main()
{
    point p { 3, 4 };
    account a { "ada", "hunter2" };
    stop();
    return p.x + (a.name != nullptr);
}

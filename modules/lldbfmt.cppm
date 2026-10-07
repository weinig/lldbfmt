// Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The lldbfmt module: the bytecode library, the debugging tools and the Python-
// shaped DSL (lldbfmt/lldbfmt.h, debug.h and script.h), and, when the module is
// built by a compiler with C++26 reflection (GCC 16 with -freflection), the
// reflection layer (lldbfmt/reflect.h) too.
//
//   import lldbfmt;
//   #include <lldbfmt/macros.h>   // macros cannot be exported by a module
//
// The settings in macros.h (LLDBFMT_MAX_BYTES and so on) are the ones the
// module was built with. Code that imports the module must be compiled with
// the same language options as the module (-std, -freflection).

module;

// What the headers use, included here so that it is not part of the module.
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(__cpp_impl_reflection)
#include <meta>
#include <optional>
#include <span>
#include <string>
#include <vector>
#endif

export module lldbfmt;

#define LLDBFMT_MODULE_BUILD 1
#include "lldbfmt/macros.h"

export {
#include "lldbfmt/lldbfmt.h"
#include "lldbfmt/script.h"
#if defined(__cpp_impl_reflection)
#include "lldbfmt/reflect.h"
#endif
}

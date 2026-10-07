# Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.28)
project(lldbfmt LANGUAGES CXX)

# The headers: link lldbfmt::lldbfmt and #include <lldbfmt/script.h> and so on.
add_library(lldbfmt INTERFACE)
add_library(lldbfmt::lldbfmt ALIAS lldbfmt)
target_include_directories(lldbfmt INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(lldbfmt INTERFACE cxx_std_20)

# The module: link lldbfmt::module, `import lldbfmt;`, and #include
# <lldbfmt/macros.h> for the macros. Needs a compiler and generator that support
# C++20 modules (Ninja, with GCC 14 or later or an LLVM clang with
# clang-scan-deps, but not Apple's clang). Built with reflection, the module includes the reflection
# layer: configure with -DCMAKE_CXX_FLAGS=-freflection and C++26 under GCC 16.
option(LLDBFMT_BUILD_MODULE "Build the lldbfmt C++ module" OFF)
if(LLDBFMT_BUILD_MODULE)
    add_library(lldbfmt_module)
    add_library(lldbfmt::module ALIAS lldbfmt_module)
    target_sources(lldbfmt_module PUBLIC
        FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules FILES modules/lldbfmt.cppm)
    target_link_libraries(lldbfmt_module PUBLIC lldbfmt::lldbfmt)
    target_compile_features(lldbfmt_module PUBLIC cxx_std_20)
endif()

install(DIRECTORY include/lldbfmt DESTINATION include)

# Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

CXX ?= clang++
LLDB ?= lldb
# A compiler with C++26 reflection (GCC 16, clang has none yet).
REFLECT_CXX ?= /opt/homebrew/bin/g++-16
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -fno-exceptions -Iinclude


.PHONY: test python-test verify-test module-test debugging-demo translate-test reflect-test gcc-test examples reflection-example example webkit-example webkit-example-lowlevel clean
test: python-test verify-test
	$(CXX) $(CXXFLAGS) -fsyntax-only tests/static_tests.cpp
	$(CXX) $(CXXFLAGS) -std=c++23 -fsyntax-only tests/static_tests.cpp
	$(CXX) $(CXXFLAGS) -fsyntax-only tests/script_tests.cpp
	$(CXX) $(CXXFLAGS) -std=c++23 -fsyntax-only tests/script_tests.cpp

python-test:
	python3 tools/formatter_bytecode.py --test

# Translates Python formatters to C++, builds them, and runs them in LLDB
# (needs a recent one: make translate-test LLDB=/path/to/lldb).
verify-test:
	python3 tests/run_verify_tests.py

# The library under GCC, a second compiler (needs REFLECT_CXX).
gcc-test:
	$(REFLECT_CXX) -std=c++26 -Wall -Wextra -Wpedantic -Iinclude -fsyntax-only tests/static_tests.cpp
	$(REFLECT_CXX) -std=c++26 -Wall -Wextra -Wpedantic -Iinclude -fsyntax-only tests/script_tests.cpp

# The C++20 module, built with clang (needs a clang with named-module support,
# not Apple's: make module-test MODULE_CXX=/path/to/clang++) and with GCC 16
# including reflection, then run in LLDB.
MODULE_CXX ?= clang++
module-test:
	python3 tests/run_module_tests.py --clang $(MODULE_CXX) --gcc $(REFLECT_CXX) --lldb $(LLDB)

# The reflection layer: compile errors for bad annotations, and a program in LLDB.
reflect-test:
	python3 tests/run_reflect_tests.py --cxx $(REFLECT_CXX) --lldb $(LLDB)

# Builds every example that is a program and shows its variables in LLDB. The
# module examples are built and run by module-test, and the deliberately
# failing ones by debugging-demo.
examples: example webkit-example webkit-example-lowlevel reflection-example

build:
	mkdir -p build

reflection-example: | build
	$(REFLECT_CXX) -std=c++26 -freflection -g -Wno-attributes -Iinclude examples/reflection.cpp -o build/reflection
	$(LLDB) -b -o 'b stop' -o run -o up -o 'frame variable' build/reflection

# Every deliberately failing case in examples/debugging*.cpp, with what the compiler says.
debugging-demo:
	python3 tools/debugging_demo.py --reflect-cxx $(REFLECT_CXX)

translate-test:
	python3 tests/run_translate_tests.py --lldb $(LLDB)

example: | build
	$(CXX) $(CXXFLAGS) -g examples/example.cpp -o build/example
	$(LLDB) -b -o 'b stop' -o run -o up -o 'frame variable' build/example

webkit-example: | build
	$(CXX) $(CXXFLAGS) -g examples/webkit_formatters.cpp -o build/webkit_formatters
	$(LLDB) -b -o 'b stop' -o run -o up -o 'frame variable' build/webkit_formatters

webkit-example-lowlevel: | build
	$(CXX) $(CXXFLAGS) -g examples/webkit_formatters_lowlevel.cpp -o build/webkit_formatters_lowlevel
	$(LLDB) -b -o 'b stop' -o run -o up -o 'frame variable' build/webkit_formatters_lowlevel

clean:
	rm -rf build

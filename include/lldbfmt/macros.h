// Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// lldbfmt/macros.h: every macro of the library: the settings that can be
// changed before including anything, the section the records go in, and the
// macros that define formatters.
//
// The other headers include this one. A module cannot export macros, so
// code that uses `import lldbfmt;` includes it itself, for the macros. The
// settings (LLDBFMT_MAX_BYTES and so on) are fixed when the module is built.

#pragma once

// Upper bound, in bytes, for a single program and for a whole serialized
// record. Exceeding it is a compile-time error. Define before including to
// raise it.
#ifndef LLDBFMT_MAX_BYTES
#define LLDBFMT_MAX_BYTES 8192
#endif

// Behaviors of the interpreter the verifier (lldbfmt/debug.h) and the
// Script DSL (lldbfmt/script.h) account for. Define these to 1 when
// targeting an LLDB that has fixed the problem.
//
// @fmt leaves its arguments on the stack beneath the result (the spec says it
// consumes them).
#ifndef LLDBFMT_FMT_CONSUMES_ARGUMENTS
#define LLDBFMT_FMT_CONSUMES_ARGUMENTS 0
#endif
// @fmt prints a bogus pointer for a UInt or Int argument.
#ifndef LLDBFMT_FMT_FORMATS_INTEGERS_CORRECTLY
#define LLDBFMT_FMT_FORMATS_INTEGERS_CORRECTLY 0
#endif
// A conditional that runs inside another conditional's block makes the
// interpreter drop the code after the outer one: resuming the outer block
// overwrites the continuation below it with an empty one.
#ifndef LLDBFMT_ALLOW_NESTED_CONDITIONALS
#define LLDBFMT_ALLOW_NESTED_CONDITIONALS 0
#endif

// Set to 1 to print the disassembly of every record the macros define, as a
// compile-time message (see lldbfmt/debug.h).
#ifndef LLDBFMT_DUMP_ALL
#define LLDBFMT_DUMP_ALL 0
#endif

// Records go in the formatter section. The older summary section only
// understands summary strings.
#if defined(__APPLE__)
#define LLDBFMT_SECTION_NAME "__DATA_CONST,__lldbformatters"
#elif defined(_WIN32)
#define LLDBFMT_SECTION_NAME ".lldbfor"
#else
#define LLDBFMT_SECTION_NAME ".lldbformatters"
#endif

// Places a record in the formatter section and keeps the linker from dropping it.
#if defined(__has_attribute)
#if __has_attribute(retain)
#define LLDBFMT_ATTRIBUTES __attribute__((used, retain, section(LLDBFMT_SECTION_NAME)))
#endif
#endif
#ifndef LLDBFMT_ATTRIBUTES
#define LLDBFMT_ATTRIBUTES __attribute__((used, section(LLDBFMT_SECTION_NAME)))
#endif

// Capacity, in characters, of the text the debugging tools build (disassembly
// and diagnostics). Longer text is cut off with "...".
#ifndef LLDBFMT_MAX_TEXT
#define LLDBFMT_MAX_TEXT 16384
#endif

// Bit width used when a UInt has to become an Integer. Values must be below
// 2^bits. Each conversion costs about 20 bytes of bytecode per bit.
#ifndef LLDBFMT_PROMOTION_BITS
#define LLDBFMT_PROMOTION_BITS 32
#endif

// A static_assert whose message is computed (C++26, which clang also takes in
// earlier modes). This is how the verifier prints a program's disassembly.
#if defined(__clang__)
#define LLDBFMT_STATIC_ASSERT(condition, message) \
    _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wc++26-extensions\"") \
    static_assert(condition, message); \
    _Pragma("clang diagnostic pop")
#elif defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
#define LLDBFMT_STATIC_ASSERT(condition, message) static_assert(condition, message);
#else
#define LLDBFMT_STATIC_ASSERT(condition, message) \
    static_assert(condition, "lldbfmt: a record does not verify or was dumped (use clang or C++26 for the details)");
#endif

// Defines `Name`, a constexpr byte array holding the serialized record, placed
// in the formatter section. `Name_record` is the same record as a `byte_buffer`.
// `RecordExpression` is evaluated at compile time with `Namespace` in scope.
// Use at namespace scope; terminate with a semicolon.
//
// A record that fails verification stops the build with the program
// disassembled and the offending instruction marked.
#define LLDBFMT_RECORD_IN(Name, Namespace, RecordExpression) \
    static constexpr ::lldbfmt::byte_buffer Name##_record = [] { \
        using namespace Namespace; \
        return RecordExpression; \
    }(); \
    LLDBFMT_STATIC_ASSERT(Name##_record.problem == 0, ::lldbfmt::diagnose(Name##_record)) \
    LLDBFMT_STATIC_ASSERT(!LLDBFMT_DUMP_ALL, ::lldbfmt::disassemble(Name##_record)) \
    LLDBFMT_ATTRIBUTES static constexpr auto Name = ::lldbfmt::detail::exact_bytes<Name##_record>()

// Prints a record's disassembly as a compile-time message (an error, so the
// build stops, but clang keeps going and reports the rest):
//   LLDBFMT_DUMP(Name);
#define LLDBFMT_DUMP(Name) \
    LLDBFMT_PRINT_RECORD(Name##_record)

// Prints any program or record `byte_buffer` expression, e.g. while writing assembly:
//   LLDBFMT_PRINT(::lldbfmt::assemble("dup 1u +"));
#define LLDBFMT_PRINT(Expression) \
    LLDBFMT_STATIC_ASSERT(sizeof(int) == 0, ::lldbfmt::disassemble_program(Expression)) \
    static_assert(true)

// Prints a record `byte_buffer` (such as `Name_record`, or one that
// lldbfmt/reflect.h defines) as a compile-time message:
//   LLDBFMT_PRINT_RECORD(point_record);
#define LLDBFMT_PRINT_RECORD(Expression) \
    LLDBFMT_STATIC_ASSERT(sizeof(int) == 0, ::lldbfmt::disassemble(Expression)) \
    static_assert(true)

// A record from any constexpr expression yielding a `byte_buffer`, with the
// low-level DSL (lldbfmt::dsl) in scope.
#define LLDBFMT_RECORD(Name, RecordExpression) \
    LLDBFMT_RECORD_IN(Name, ::lldbfmt::dsl, RecordExpression)

// LLDBFMT_FORMATTER(Name, "TypeNameOrRegex", flags, methods...)
//   LLDBFMT_FORMATTER(Point, "Point", 0,
//       summary(dup, "x", selector::get_child_with_name, call, ...));
#define LLDBFMT_FORMATTER(Name, TypeName, Flags, ...) \
    LLDBFMT_RECORD(Name, ::lldbfmt::record(TypeName, Flags, __VA_ARGS__))

// LLDBFMT_ASSEMBLY(Name, "TypeNameOrRegex", flags, "@summary: ...")
#define LLDBFMT_ASSEMBLY(Name, TypeName, Flags, Assembly) \
    LLDBFMT_RECORD(Name, ::lldbfmt::record_from_assembly(TypeName, Flags, Assembly))

// LLDBFMT_SCRIPT_SUMMARY(Name, "Type", [](auto valobj) { ... });
// LLDBFMT_SCRIPT_SYNTHETIC(Name, "Type", update([](auto valobj) { ... }), num_children(...), ...);
// A type name starting with '^' is a regex. Use at namespace scope. The
// formatters cascade (see type_options). The _WITH_FLAGS forms take other options.
#define LLDBFMT_SCRIPT_SUMMARY(Name, TypeName, ...) \
    LLDBFMT_SCRIPT_SUMMARY_WITH_FLAGS(Name, TypeName, ::lldbfmt::type_options::cascade, __VA_ARGS__)

#define LLDBFMT_SCRIPT_SUMMARY_WITH_FLAGS(Name, TypeName, Flags, ...) \
    LLDBFMT_RECORD_IN(Name, ::lldbfmt::script, ::lldbfmt::script::summary(TypeName, Flags, __VA_ARGS__))

#define LLDBFMT_SCRIPT_SYNTHETIC(Name, TypeName, ...) \
    LLDBFMT_SCRIPT_SYNTHETIC_WITH_FLAGS(Name, TypeName, ::lldbfmt::type_options::cascade, __VA_ARGS__)

#define LLDBFMT_SCRIPT_SYNTHETIC_WITH_FLAGS(Name, TypeName, Flags, ...) \
    LLDBFMT_RECORD_IN(Name, ::lldbfmt::script, ::lldbfmt::script::synthetic(TypeName, Flags, __VA_ARGS__))

// Pastes two tokens after expanding them.
#define LLDBFMT_REFLECT_CONCAT_(a, b) a##b
#define LLDBFMT_REFLECT_CONCAT(a, b) LLDBFMT_REFLECT_CONCAT_(a, b)

// Generates and embeds the formatters that Type's annotations describe: a
// summary if it has a `summary`, and children if it has a `hidden`, `rename` or
// `array` member. Use at namespace scope; terminate with a semicolon. For a
// class template, give a specialization, through an alias if it has commas:
//   using IntBuffer = Buffer<int>;
//   LLDBFMT_REFLECT(IntBuffer);
#define LLDBFMT_REFLECT(Type) LLDBFMT_REFLECT_WITH_ID(Type, __COUNTER__)
#define LLDBFMT_REFLECT_WITH_ID(Type, Id) LLDBFMT_REFLECT_NAMED(Type, LLDBFMT_REFLECT_CONCAT(lldbfmt_reflected_, Id))

// The same, naming the variables: `Name_summary_record` and
// `Name_synthetic_record` are the records as `byte_buffer`s (for
// LLDBFMT_PRINT_RECORD and tests), and `Name` is the array in the section.
#define LLDBFMT_REFLECT_NAMED(Type, Name) \
    LLDBFMT_STATIC_ASSERT(::lldbfmt::reflect::impl::type_formatter<Type>::error.empty(), ::lldbfmt::reflect::impl::type_formatter<Type>::error) \
    static constexpr ::lldbfmt::byte_buffer LLDBFMT_REFLECT_CONCAT(Name, _summary_record) = ::lldbfmt::reflect::impl::summary_record_or_nothing<Type>(); \
    static constexpr ::lldbfmt::byte_buffer LLDBFMT_REFLECT_CONCAT(Name, _synthetic_record) = ::lldbfmt::reflect::impl::synthetic_record_or_nothing<Type>(); \
    LLDBFMT_STATIC_ASSERT(LLDBFMT_REFLECT_CONCAT(Name, _summary_record).problem == 0, ::lldbfmt::diagnose(LLDBFMT_REFLECT_CONCAT(Name, _summary_record))) \
    LLDBFMT_STATIC_ASSERT(LLDBFMT_REFLECT_CONCAT(Name, _synthetic_record).problem == 0, ::lldbfmt::diagnose(LLDBFMT_REFLECT_CONCAT(Name, _synthetic_record))) \
    LLDBFMT_STATIC_ASSERT(!LLDBFMT_DUMP_ALL, ::lldbfmt::disassemble(LLDBFMT_REFLECT_CONCAT(Name, _summary_record))) \
    LLDBFMT_STATIC_ASSERT(!LLDBFMT_DUMP_ALL, ::lldbfmt::disassemble(LLDBFMT_REFLECT_CONCAT(Name, _synthetic_record))) \
    LLDBFMT_ATTRIBUTES static constexpr auto Name = ::lldbfmt::reflect::impl::concatenated<LLDBFMT_REFLECT_CONCAT(Name, _summary_record), LLDBFMT_REFLECT_CONCAT(Name, _synthetic_record)>()

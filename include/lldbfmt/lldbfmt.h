// Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// lldbfmt/lldbfmt.h: header-only, consteval builder for LLDB formatter bytecode.
//
// Formatters are described in C++, compiled to LLDB formatter bytecode during
// constant evaluation, and emitted into the binary's formatter section by a
// single macro.
//
// Bytecode and record format:
//   https://lldb.llvm.org/resources/formatterbytecode.html
// Opcode, selector and signature numbering mirrors
//   lldb/include/lldb/DataFormatters/FormatterBytecode.def
//
// Requires C++20.

#pragma once

// (The module interface unit includes these itself, and defines
// LLDBFMT_MODULE_BUILD.)
#ifndef LLDBFMT_MODULE_BUILD
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#endif

#include "lldbfmt/macros.h"

namespace lldbfmt {

////////////////////////////////////////////////////////////////////////////////
// Tables. Keep in sync with FormatterBytecode.def.
////////////////////////////////////////////////////////////////////////////////

// X(value, mnemonic, Name). Opcodes without a textual mnemonic are the
// literal-introducing ones, which the assembler spells via their operand.
#define LLDBFMT_OPCODES(X) \
    X(0x01, "dup", dup) \
    X(0x02, "drop", drop) \
    X(0x03, "pick", pick) \
    X(0x04, "over", over) \
    X(0x05, "swap", swap) \
    X(0x06, "rot", rot) \
    X(0x10, "{", begin) \
    X(0x11, "if", if_) \
    X(0x12, "ifelse", if_else) \
    X(0x13, "return", return_) \
    X(0x20, "", lit_uint) \
    X(0x21, "", lit_int) \
    X(0x22, "", lit_string) \
    X(0x23, "", lit_selector) \
    X(0x24, "", lit_integer) \
    X(0x25, "null", lit_null) \
    X(0x2a, "as_int", as_int) \
    X(0x2b, "as_uint", as_uint) \
    X(0x2c, "is_null", is_null) \
    X(0x30, "+", plus) \
    X(0x31, "-", minus) \
    X(0x32, "*", mul) \
    X(0x33, "/", div) \
    X(0x34, "%", mod) \
    X(0x35, "<<", shl) \
    X(0x36, ">>", shr) \
    X(0x40, "&", and_) \
    X(0x41, "|", or_) \
    X(0x42, "^", xor_) \
    X(0x43, "~", not_) \
    X(0x50, "=", eq) \
    X(0x51, "!=", neq) \
    X(0x52, "<", lt) \
    X(0x53, ">", gt) \
    X(0x54, "=<", le) \
    X(0x55, ">=", ge) \
    X(0x60, "call", call) \
    X(0x70, "dict", dict) \
    X(0x71, "dict_set", dict_set) \
    X(0x72, "dict_get", dict_get) \
    X(0x73, "dict_has", dict_has)

#define LLDBFMT_SELECTORS(X) \
    X(0x00, "summary", summary) \
    X(0x01, "type_summary", type_summary) \
    X(0x10, "get_num_children", get_num_children) \
    X(0x11, "get_child_at_index", get_child_at_index) \
    X(0x12, "get_child_with_name", get_child_with_name) \
    X(0x13, "get_child_index", get_child_index) \
    X(0x14, "get_parent", get_parent) \
    X(0x15, "get_type", get_type) \
    X(0x16, "get_template_argument_type", get_template_argument_type) \
    X(0x17, "cast", cast) \
    X(0x18, "get_synthetic_value", get_synthetic_value) \
    X(0x19, "get_non_synthetic_value", get_non_synthetic_value) \
    X(0x20, "get_value", get_value) \
    X(0x21, "get_value_as_unsigned", get_value_as_unsigned) \
    X(0x22, "get_value_as_signed", get_value_as_signed) \
    X(0x23, "get_value_as_address", get_value_as_address) \
    X(0x24, "clone", clone) \
    X(0x25, "get_pointee_type", get_pointee_type) \
    X(0x26, "get_byte_size", get_byte_size) \
    X(0x27, "create_child_at_offset", create_child_at_offset) \
    X(0x40, "read_memory_byte", read_memory_byte) \
    X(0x41, "read_memory_uint32", read_memory_uint32) \
    X(0x42, "read_memory_int32", read_memory_int32) \
    X(0x43, "read_memory_unsigned", read_memory_unsigned) \
    X(0x44, "read_memory_signed", read_memory_signed) \
    X(0x45, "read_memory_address", read_memory_address) \
    X(0x46, "read_memory", read_memory) \
    X(0x50, "fmt", fmt) \
    X(0x51, "sprintf", sprintf) \
    X(0x52, "strlen", strlen)

#define LLDBFMT_SIGNATURES(X) \
    X(0, "summary", summary) \
    X(1, "init", init) \
    X(2, "get_num_children", get_num_children) \
    X(3, "get_child_index", get_child_index) \
    X(4, "get_child_at_index", get_child_at_index) \
    X(5, "get_value", get_value) \
    X(6, "update", update)

#define LLDBFMT_ENUMERATOR(value, mnemonic, Name) Name = value,
#define LLDBFMT_OPCODE_ROW(value, mnemonic, Name) { mnemonic, opcode::Name },
#define LLDBFMT_SELECTOR_ROW(value, mnemonic, Name) { mnemonic, selector::Name },
#define LLDBFMT_SIGNATURE_ROW(value, mnemonic, Name) { mnemonic, signature::Name },

enum class opcode : uint8_t { LLDBFMT_OPCODES(LLDBFMT_ENUMERATOR) };
enum class selector : uint8_t { LLDBFMT_SELECTORS(LLDBFMT_ENUMERATOR) };
enum class signature : uint8_t { LLDBFMT_SIGNATURES(LLDBFMT_ENUMERATOR) };

// lldb::TypeOptions, the `flags` of a record (lldb-enumerations.h). Without
// `cascade`, LLDB lists the formatter as "(not cascading)" and applies it only
// to the exact type name, not through a typedef or `using` alias, and not to
// classes derived from it. `type summary add` cascades unless told not to, and
// so do the Script and Reflection macros.
namespace type_options {

inline constexpr uint64_t none = 0;
inline constexpr uint64_t cascade = 1 << 0;
inline constexpr uint64_t skip_pointers = 1 << 1;
inline constexpr uint64_t skip_references = 1 << 2;
inline constexpr uint64_t hide_children = 1 << 3;
inline constexpr uint64_t hide_value = 1 << 4;
inline constexpr uint64_t show_one_liner = 1 << 5;
inline constexpr uint64_t hide_names = 1 << 6;
inline constexpr uint64_t non_cacheable = 1 << 7;
inline constexpr uint64_t hide_empty_aggregates = 1 << 8;
inline constexpr uint64_t front_end_wants_dereference = 1 << 9;
inline constexpr uint64_t custom_subscripting = 1 << 10;

} // namespace type_options

inline constexpr size_t signature_count = 7;
inline constexpr uint8_t record_version = 1;

////////////////////////////////////////////////////////////////////////////////
// Diagnostics.
////////////////////////////////////////////////////////////////////////////////

// Compile errors are produced by calling `fail`, which is deliberately not
// constexpr (and not defined): reaching it during constant evaluation is a
// compile error whose note names the enumerator below. Because everything that
// builds a formatter runs in a constant expression, it can never be reached at
// run time.
enum class error {
    buffer_overflow,               // Raise LLDBFMT_MAX_BYTES.
    block_nesting_too_deep,
    invalid_number,                // Malformed or out-of-range integer literal.
    unknown_mnemonic,
    unknown_selector,
    unknown_signature,
    unterminated_string,
    unmatched_block_open,          // '{' without '}'.
    unmatched_block_close,         // '}' without '{'.
    unexpected_signature_label,    // '@summary:' in a plain program.
    tokens_before_first_signature_label,
    duplicate_signature,
    summary_mixed_with_synthetic_methods, // LLDB would silently drop the synthetic methods.
    empty_type_name,
    no_programs,
    format_specifier_count_mismatch, // lldbfmt/script.h: "%s" count differs from the arguments.
    unsupported_format_specifier,  // lldbfmt/script.h: only %s %d %i %u %% are understood.
    negative_unsigned_literal,     // lldbfmt/script.h: a negative literal where a UInt is needed.
    nested_conditional_not_last,   // lldbfmt/script.h: an interpreter bug would drop the code after it.
};

[[noreturn]] void fail(error);

////////////////////////////////////////////////////////////////////////////////
// Byte buffer with LEB128 support.
////////////////////////////////////////////////////////////////////////////////

constexpr size_t uleb_size(uint64_t value)
{
    size_t size = 1;
    while (value >>= 7)
        ++size;
    return size;
}

struct byte_buffer {
    static constexpr size_t capacity = LLDBFMT_MAX_BYTES;
    static constexpr size_t max_block_depth = 64;

    std::array<uint8_t, capacity> data { };
    size_t size { 0 };

    // Set on a serialized record whose program failed verification
    // (lldbfmt/debug.h): what, where, and the types involved.
    uint8_t problem { 0 };
    uint8_t problem_signature { 0 };
    size_t problem_offset { 0 };
    uint16_t problem_expected { 0 };
    uint16_t problem_actual { 0 };

    constexpr void append(uint8_t byte)
    {
        if (size == capacity)
            fail(error::buffer_overflow);
        data[size++] = byte;
    }

    constexpr void append(opcode code)
    {
        append(static_cast<uint8_t>(code));
    }

    constexpr void append(std::string_view string)
    {
        for (char character : string)
            append(static_cast<uint8_t>(character));
    }

    constexpr void append(const byte_buffer& other)
    {
        for (size_t i = 0; i < other.size; ++i)
            append(other.data[i]);
    }

    constexpr void append_uleb(uint64_t value)
    {
        do {
            uint8_t byte = value & 0x7f;
            value >>= 7;
            if (value)
                byte |= 0x80;
            append(byte);
        } while (value);
    }

    constexpr void append_sleb(int64_t value)
    {
        bool more = true;
        while (more) {
            uint8_t byte = value & 0x7f;
            value >>= 7; // Arithmetic shift (guaranteed since C++20).
            bool sign_bit_set = byte & 0x40;
            more = !((value == 0 && !sign_bit_set) || (value == -1 && sign_bit_set));
            if (more)
                byte |= 0x80;
            append(byte);
        }
    }

    // A block is `{ <body> }`, encoded as `begin ULEB(length) <body>`. The
    // length prefix is variable width, so it is inserted in front of the body
    // once the body's size is known.
    constexpr void end_block(size_t body_start)
    {
        size_t length = size - body_start;
        size_t header_size = 1 + uleb_size(length);
        if (size + header_size > capacity)
            fail(error::buffer_overflow);
        for (size_t i = size; i-- > body_start;)
            data[i + header_size] = data[i];
        size += header_size;

        size_t end = size;
        size = body_start;
        append(opcode::begin);
        append_uleb(length);
        size = end;
    }
};

////////////////////////////////////////////////////////////////////////////////
// Name tables, used by the text assembler.
////////////////////////////////////////////////////////////////////////////////

namespace detail {

template<typename Enum>
struct table_entry {
    std::string_view name;
    Enum value;
};

inline constexpr table_entry<opcode> opcode_table[] = { LLDBFMT_OPCODES(LLDBFMT_OPCODE_ROW) };
inline constexpr table_entry<selector> selector_table[] = { LLDBFMT_SELECTORS(LLDBFMT_SELECTOR_ROW) };
inline constexpr table_entry<signature> signature_table[] = { LLDBFMT_SIGNATURES(LLDBFMT_SIGNATURE_ROW) };

} // namespace detail

////////////////////////////////////////////////////////////////////////////////
// Text assembler. Same syntax as the reference Python assembler
// (lldb/examples/python/formatter_bytecode.py):
//
//   123        signed integer literal (also -5, 0x1f)
//   123u       unsigned integer literal
//   "text"     string literal; \" and \\ are the only escapes
//   @name      selector literal
//   { ... }    code block
//   mnemonic   any other opcode: dup if ifelse call + - ...
//
// Unlike the Python assembler, block and string lengths and integer literals
// are LEB128-encoded, so they are not limited to 127.
////////////////////////////////////////////////////////////////////////////////

namespace detail {

constexpr bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
constexpr bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// Returns the next token starting at or after `position` and advances
// `position` past it. An empty result means end of input. A string token
// includes its quotes and escapes.
constexpr std::string_view next_token(std::string_view text, size_t& position)
{
    while (position < text.size() && is_space(text[position]))
        ++position;
    if (position == text.size())
        return { };

    size_t start = position;
    if (text[position] == '"') {
        ++position;
        while (true) {
            if (position == text.size())
                fail(error::unterminated_string);
            char c = text[position++];
            if (c == '\\') {
                if (position == text.size())
                    fail(error::unterminated_string);
                ++position;
            } else if (c == '"')
                break;
        }
    } else {
        while (position < text.size() && !is_space(text[position]))
            ++position;
    }
    return text.substr(start, position - start);
}

constexpr uint64_t parse_magnitude(std::string_view digits)
{
    uint64_t base = 10;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
    }
    if (digits.empty())
        fail(error::invalid_number);

    uint64_t value = 0;
    for (char c : digits) {
        uint64_t digit;
        if (is_digit(c))
            digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            fail(error::invalid_number);
        if (value > (UINT64_MAX - digit) / base)
            fail(error::invalid_number);
        value = value * base + digit;
    }
    return value;
}

constexpr void emit_unsigned_literal(byte_buffer& out, uint64_t value)
{
    out.append(opcode::lit_uint);
    out.append_uleb(value);
}

constexpr void emit_integer_literal(byte_buffer& out, int64_t value)
{
    out.append(opcode::lit_integer);
    out.append_sleb(value);
}

constexpr void emit_string_literal(byte_buffer& out, std::string_view string)
{
    out.append(opcode::lit_string);
    out.append_uleb(string.size());
    out.append(string);
}

constexpr void emit_selector_literal(byte_buffer& out, selector sel)
{
    out.append(opcode::lit_selector);
    out.append(static_cast<uint8_t>(sel));
}

constexpr void assemble_token(byte_buffer& out, std::string_view token, size_t block_starts[], size_t& block_depth)
{
    char first = token[0];

    if (token == "{") {
        if (block_depth == byte_buffer::max_block_depth)
            fail(error::block_nesting_too_deep);
        block_starts[block_depth++] = out.size;
        return;
    }
    if (token == "}") {
        if (!block_depth)
            fail(error::unmatched_block_close);
        out.end_block(block_starts[--block_depth]);
        return;
    }

    if (first == '"') {
        // Strip the quotes, then the backslash escaping '"' and '\'.
        std::string_view body = token.substr(1, token.size() - 2);
        out.append(opcode::lit_string);
        size_t length = 0;
        for (size_t i = 0; i < body.size(); ++i, ++length) {
            if (body[i] == '\\')
                ++i;
        }
        out.append_uleb(length);
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == '\\')
                ++i;
            out.append(static_cast<uint8_t>(body[i]));
        }
        return;
    }

    if (first == '@') {
        std::string_view name = token.substr(1);
        for (const auto& entry : selector_table) {
            if (entry.name == name) {
                emit_selector_literal(out, entry.value);
                return;
            }
        }
        fail(error::unknown_selector);
    }

    bool negative = first == '-' && token.size() > 1 && is_digit(token[1]);
    if (is_digit(first) || negative) {
        std::string_view digits = negative ? token.substr(1) : token;
        bool is_unsigned = digits.back() == 'u';
        if (is_unsigned) {
            if (negative)
                fail(error::invalid_number);
            digits.remove_suffix(1);
            emit_unsigned_literal(out, parse_magnitude(digits));
            return;
        }
        uint64_t magnitude = parse_magnitude(digits);
        if (negative) {
            if (magnitude > static_cast<uint64_t>(INT64_MAX) + 1)
                fail(error::invalid_number);
            emit_integer_literal(out, static_cast<int64_t>(0 - magnitude));
        } else {
            if (magnitude > static_cast<uint64_t>(INT64_MAX))
                fail(error::invalid_number);
            emit_integer_literal(out, static_cast<int64_t>(magnitude));
        }
        return;
    }

    for (const auto& entry : opcode_table) {
        // The empty mnemonic marks literal opcodes. "{" was handled above.
        if (!entry.name.empty() && entry.value != opcode::begin && entry.name == token) {
            out.append(entry.value);
            return;
        }
    }
    fail(error::unknown_mnemonic);
}

constexpr bool is_signature_label(std::string_view token, signature& sig)
{
    if (token.size() < 3 || token[0] != '@' || token.back() != ':')
        return false;
    std::string_view name = token.substr(1, token.size() - 2);
    for (const auto& entry : signature_table) {
        if (entry.name == name) {
            sig = entry.value;
            return true;
        }
    }
    fail(error::unknown_signature);
}

constexpr void assemble_into(byte_buffer& out, std::string_view text)
{
    size_t block_starts[byte_buffer::max_block_depth] = { };
    size_t block_depth = 0;
    size_t position = 0;
    while (true) {
        std::string_view token = next_token(text, position);
        if (token.empty())
            break;
        signature ignored;
        if (token.back() == ':' && is_signature_label(token, ignored))
            fail(error::unexpected_signature_label);
        assemble_token(out, token, block_starts, block_depth);
    }
    if (block_depth)
        fail(error::unmatched_block_open);
}

} // namespace detail

// Assembles one program.
constexpr byte_buffer assemble(std::string_view text)
{
    byte_buffer out;
    detail::assemble_into(out, text);
    return out;
}

} // namespace lldbfmt

// debug.h needs everything above, and the records below need debug.h.
#include "lldbfmt/debug.h"

namespace lldbfmt {

////////////////////////////////////////////////////////////////////////////////
// Records.
////////////////////////////////////////////////////////////////////////////////

struct method_program {
    signature sig;
    byte_buffer bytecode;
};

namespace detail {

struct program_set {
    std::array<byte_buffer, signature_count> bytecode { };
    std::array<bool, signature_count> present { };
    std::array<slot_types, signature_count> slots { };

    constexpr void set(signature sig, const byte_buffer& program)
    {
        size_t index = static_cast<size_t>(sig);
        if (present[index])
            fail(error::duplicate_signature);
        present[index] = true;
        bytecode[index] = program;
        // A summary or init starts with just the object. A getter also has
        // the attributes and an argument, so what is on the stack is unknown.
        bool known = sig == signature::summary || sig == signature::init;
        slots[index].kinds[0] = kinds::object;
        slots[index].count = known ? 1 : 0;
        slots[index].open = !known;
    }

    // `types` describes the values the program starts with.
    constexpr void set(signature sig, const byte_buffer& program, const slot_types& types)
    {
        set(sig, program);
        slots[static_cast<size_t>(sig)] = types;
    }
};

constexpr byte_buffer serialize(std::string_view type_name, uint64_t flags, const program_set& programs)
{
    if (type_name.empty())
        fail(error::empty_type_name);

    bool has_summary = programs.present[static_cast<size_t>(signature::summary)];
    bool has_synthetic = false;
    for (size_t i = 1; i < signature_count; ++i)
        has_synthetic |= programs.present[i];
    if (!has_summary && !has_synthetic)
        fail(error::no_programs);
    if (has_summary && has_synthetic)
        fail(error::summary_mixed_with_synthetic_methods);

    byte_buffer body;
    body.append_uleb(type_name.size());
    body.append(type_name);
    body.append_uleb(flags);
    for (size_t i = 0; i < signature_count; ++i) {
        if (!programs.present[i])
            continue;
        body.append(static_cast<uint8_t>(i));
        body.append_uleb(programs.bytecode[i].size);
        body.append(programs.bytecode[i]);
    }

    byte_buffer record;
    record.append_uleb(record_version);
    record.append_uleb(body.size);
    record.append(body);

    // Check every program the way LLDB's interpreter would run it, and record
    // the first failure in the result.
    for (size_t i = 0; i < signature_count && !record.problem; ++i) {
        if (!programs.present[i])
            continue;
        verify_result result = verify(programs.bytecode[i], static_cast<signature>(i), programs.slots[i]);
        if (result.problem != problem_kind::none) {
            record.problem = static_cast<uint8_t>(result.problem);
            record.problem_signature = static_cast<uint8_t>(i);
            record.problem_offset = result.offset;
            record.problem_expected = result.expected;
            record.problem_actual = result.actual;
        }
    }
    return record;
}

} // namespace detail

// Serializes one record: version, size, type name (a regex if it starts with
// '^'), flags (an lldb::TypeOptions bitfield, see type_options), and the
// methods. A record holds either a @summary or synthetic-children methods, so
// use two records for both.
template<typename... Methods>
constexpr byte_buffer record(std::string_view type_name, uint64_t flags, const Methods&... methods)
{
    detail::program_set programs;
    (programs.set(methods.sig, methods.bytecode), ...);
    return detail::serialize(type_name, flags, programs);
}

// Like `record`, but from assembly text with `@signature:` labels:
//
//   @summary: dup "x" @get_child_with_name call ... return
//   @get_num_children: drop 2u return
constexpr byte_buffer record_from_assembly(std::string_view type_name, uint64_t flags, std::string_view text)
{
    detail::program_set programs;
    bool in_section = false;
    signature current { };
    size_t body_start = 0;
    size_t position = 0;

    auto finish_section = [&](size_t body_end) {
        if (!in_section)
            return;
        byte_buffer program;
        detail::assemble_into(program, text.substr(body_start, body_end - body_start));
        programs.set(current, program);
    };

    while (true) {
        std::string_view token = detail::next_token(text, position);
        if (token.empty()) {
            finish_section(text.size());
            break;
        }
        // next_token skipped leading whitespace, so the token ends at `position`.
        size_t token_start = position - token.size();

        signature sig { };
        if (token.back() == ':' && detail::is_signature_label(token, sig)) {
            finish_section(token_start);
            current = sig;
            in_section = true;
            body_start = position;
        } else if (!in_section)
            fail(error::tokens_before_first_signature_label);
    }
    return detail::serialize(type_name, flags, programs);
}

////////////////////////////////////////////////////////////////////////////////
// Embedded DSL. Programs are written as C++ expressions that read like the
// assembly:
//
//   summary(dup, "x", selector::get_child_with_name, call, 3u, pick, return_)
//
// Elements of a program:
//   123        signed integer literal         (assembly: 123)
//   123u       unsigned integer literal       (assembly: 123u)
//   "text"     string literal
//   selector::x  selector literal             (assembly: @x)
//   dup, call, ...  opcodes
//   block(...)  code block                    (assembly: { ... })
//   if_then(block(...)), if_then_else(block(...), block(...))
//
// Methods, one per signature: summary, init, get_num_children, get_child_index,
// get_child_at_index, get_value, update.
//
// `pick` and the shift operators take an unsigned count: write `3u, pick`.
// `pick` counts from the bottom of the data stack, not the top.
// `uint_to_integer<Bits>()` converts a UInt on the stack to an Integer.
////////////////////////////////////////////////////////////////////////////////

namespace dsl {

struct op {
    opcode code;
    constexpr void emit(byte_buffer& out) const
    {
        out.append(code);
    }
};

struct uint_literal {
    uint64_t value;
    constexpr void emit(byte_buffer& out) const
    {
        detail::emit_unsigned_literal(out, value);
    }
};

struct integer_literal {
    int64_t value;
    constexpr void emit(byte_buffer& out) const
    {
        detail::emit_integer_literal(out, value);
    }
};

struct string_literal {
    std::string_view value;
    constexpr void emit(byte_buffer& out) const
    {
        detail::emit_string_literal(out, value);
    }
};

struct selector_literal {
    selector value;
    constexpr void emit(byte_buffer& out) const
    {
        detail::emit_selector_literal(out, value);
    }
};

template<typename T>
concept element_like = requires(const T& element, byte_buffer& out) { element.emit(out); };

// Conversion of plain C++ values to elements.
template<element_like T>
constexpr const T& to_element(const T& element)
{
    return element;
}

template<std::integral T>
    requires (!std::same_as<T, bool>)
constexpr auto to_element(T value)
{
    if constexpr (std::is_unsigned_v<T>)
        return uint_literal { value };
    else
        return integer_literal { value };
}

template<size_t N>
constexpr string_literal to_element(const char (&string)[N])
{
    return { std::string_view(string, N - 1) };
}

constexpr string_literal to_element(std::string_view string)
{
    return { string };
}

constexpr selector_literal to_element(selector sel)
{
    return { sel };
}

template<typename... Elements>
struct sequence_node {
    std::tuple<Elements...> elements;

    constexpr void emit(byte_buffer& out) const
    {
        std::apply([&](const Elements&... element) { (element.emit(out), ...); }, elements);
    }
};

template<typename... Arguments>
constexpr auto sequence(const Arguments&... arguments)
{
    return sequence_node<std::remove_cvref_t<decltype(to_element(arguments))>...> { std::make_tuple(to_element(arguments)...) };
}

template<typename... Elements>
struct block_node {
    sequence_node<Elements...> body;

    constexpr void emit(byte_buffer& out) const
    {
        size_t start = out.size;
        body.emit(out);
        out.end_block(start);
    }
};

template<typename... Arguments>
constexpr auto block(const Arguments&... arguments)
{
    auto body = sequence(arguments...);
    return []<typename... Elements>(sequence_node<Elements...> nodes) { return block_node<Elements...> { nodes }; }(body);
}

// `cond if_then(block(...))` runs the block when the condition is non-zero.
template<element_like Then>
constexpr auto if_then(const Then& then)
{
    return sequence(then, op { opcode::if_ });
}

// `cond if_then_else(block(then...), block(else...))`
template<element_like Then, element_like Else>
constexpr auto if_then_else(const Then& then, const Else& otherwise)
{
    return sequence(then, otherwise, op { opcode::if_else });
}

// Methods: the program for one signature.
template<typename... Arguments>
constexpr method_program method(signature sig, const Arguments&... arguments)
{
    method_program result { sig, { } };
    (to_element(arguments).emit(result.bytecode), ...);
    return result;
}

#define LLDBFMT_METHOD(value, mnemonic, Name) \
    template<typename... Arguments> \
    constexpr method_program Name(const Arguments&... arguments) \
    { \
        return method(signature::Name, arguments...); \
    }
LLDBFMT_SIGNATURES(LLDBFMT_METHOD)
#undef LLDBFMT_METHOD

// A method written as assembly text.
constexpr method_program assembled(signature sig, std::string_view text)
{
    return { sig, assemble(text) };
}

// Opcodes. Names that collide with C++ keywords, <cstdlib>, or <cmath> are
// spelled out.
inline constexpr op dup { opcode::dup };
inline constexpr op drop { opcode::drop };
inline constexpr op pick { opcode::pick };
inline constexpr op over { opcode::over };
inline constexpr op swap { opcode::swap };
inline constexpr op rot { opcode::rot };
inline constexpr op return_ { opcode::return_ };
inline constexpr op null { opcode::lit_null };
inline constexpr op as_int { opcode::as_int };
inline constexpr op as_uint { opcode::as_uint };
inline constexpr op is_null { opcode::is_null };
inline constexpr op plus { opcode::plus };
inline constexpr op minus { opcode::minus };
inline constexpr op mul { opcode::mul };
inline constexpr op divide { opcode::div };
inline constexpr op modulo { opcode::mod };
inline constexpr op shift_left { opcode::shl };
inline constexpr op shift_right { opcode::shr };
inline constexpr op bit_and { opcode::and_ };
inline constexpr op bit_or { opcode::or_ };
inline constexpr op bit_xor { opcode::xor_ };
inline constexpr op bit_not { opcode::not_ };
inline constexpr op equal { opcode::eq };
inline constexpr op not_equal { opcode::neq };
inline constexpr op less { opcode::lt };
inline constexpr op greater { opcode::gt };
inline constexpr op less_equal { opcode::le };
inline constexpr op greater_equal { opcode::ge };
inline constexpr op call { opcode::call };
inline constexpr op dict { opcode::dict };
inline constexpr op dict_set { opcode::dict_set };
inline constexpr op dict_get { opcode::dict_get };
inline constexpr op dict_has { opcode::dict_has };

// ( UInt -> Integer ). The interpreter has no conversion between its unsigned
// and arbitrary-precision types, but some selectors want an Integer
// (@create_child_at_offset) while others produce a UInt (an index), and `*`
// and friends refuse mixed operands. This builds the Integer one bit at a
// time: for each bit b of the UInt that is set, add Integer(1) << b. Integer
// literals are at least 64 bits wide, so shifts below 64 are in bounds.
//
// The value must be less than 2^Bits (Bits <= 63 so the result is the same
// mathematical value). The cost is about 20 bytes of bytecode per bit.
namespace detail {

template<size_t Bit>
constexpr auto uint_to_integer_step()
{
    // stack: acc n  ->  acc n, adding 1 << Bit to acc when bit Bit of n is set.
    return sequence(
        dup, uint64_t { Bit }, shift_right, 1u, bit_and,
        if_then(block(swap, 1, uint64_t { Bit }, shift_left, plus, swap)));
}

} // namespace detail

template<unsigned Bits = LLDBFMT_PROMOTION_BITS>
constexpr auto uint_to_integer()
{
    static_assert(Bits >= 1 && Bits <= 63, "Bits must be in [1, 63]");
    return []<size_t... Bit>(std::index_sequence<Bit...>) {
        return sequence(0, swap, detail::uint_to_integer_step<Bit>()..., drop);
    }(std::make_index_sequence<Bits>());
}

} // namespace dsl

////////////////////////////////////////////////////////////////////////////////
// Emission.
////////////////////////////////////////////////////////////////////////////////

namespace detail {

// The record as an exactly-sized array, so the emitted variable has no slack.
template<const byte_buffer& Record>
consteval auto exact_bytes()
{
    std::array<uint8_t, Record.size> result { };
    for (size_t i = 0; i < Record.size; ++i)
        result[i] = Record.data[i];
    return result;
}

} // namespace detail

} // namespace lldbfmt

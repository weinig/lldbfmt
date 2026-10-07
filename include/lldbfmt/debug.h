// Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// lldbfmt/debug.h: inspecting and checking formatters at compile time.
//
//   * A constexpr disassembler, with indented blocks.
//   * A verifier that runs a program abstractly with the same type rules as
//     LLDB's interpreter, and reports the first mistake at the instruction
//     that makes it.
//   * Compile-time "printing": computed text in a static_assert message.
//
// lldbfmt/lldbfmt.h includes this. The verifier runs on every record the
// macros define, and a failure stops the build with a message like
//
//   lldbfmt: "WebCore::IntRect": @summary does not verify
//     error: type mismatch (expected UInt or Integer, found String)
//
//     0u pick "m_x" @get_child_with_name call @get_value call
//     { ...
//         ^ here
//
// LLDBFMT_DUMP(Name) prints a record's disassembly the same way (as a
// failed static_assert, which clang reports without stopping), and
// -DLLDBFMT_DUMP_ALL=1 prints every record the macros define.
//
// Printing needs a compiler with computed static_assert messages (C++26, or
// clang in C++20 mode). Elsewhere a failure is still reported but without the
// text. Use the disassembler at run time or tools/formatter_bytecode.py
// --dump-section instead.

#pragma once

#include "lldbfmt/lldbfmt.h"

namespace lldbfmt {

////////////////////////////////////////////////////////////////////////////////
// Text buffer.
////////////////////////////////////////////////////////////////////////////////

// A fixed-capacity string, usable as a static_assert message.
struct text_buffer {
    std::array<char, LLDBFMT_MAX_TEXT> characters { };
    size_t length { 0 };

    constexpr size_t size() const
    {
        return length;
    }
    constexpr const char* data() const
    {
        return characters.data();
    }
    constexpr std::string_view view() const
    {
        return { characters.data(), length };
    }

    constexpr void append(char c)
    {
        if (length + 4 < characters.size())
            characters[length++] = c;
        else if (length + 4 == characters.size()) {
            for (char dot : { '.', '.', '.' })
                characters[length++] = dot;
            ++length; // Full: ignore the rest.
        }
    }
    constexpr void append(std::string_view text)
    {
        for (char c : text)
            append(c);
    }
    constexpr void append_number(uint64_t value)
    {
        char digits[20] { };
        size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (count)
            append(digits[--count]);
    }
    constexpr void append_signed(int64_t value)
    {
        if (value < 0) {
            append('-');
            append_number(0 - static_cast<uint64_t>(value));
        } else
            append_number(static_cast<uint64_t>(value));
    }
    constexpr void append_hex(uint8_t value)
    {
        constexpr char digits[] = "0123456789abcdef";
        append(digits[value >> 4]);
        append(digits[value & 15]);
    }
    constexpr void spaces(size_t count)
    {
        while (count--)
            append(' ');
    }
};

////////////////////////////////////////////////////////////////////////////////
// What the interpreter can hold: the types a verified value may have.
////////////////////////////////////////////////////////////////////////////////

namespace kinds {

inline constexpr uint16_t object = 1;
inline constexpr uint16_t type = 2;
inline constexpr uint16_t string = 4;
inline constexpr uint16_t unsigned_int = 8;  // UInt
inline constexpr uint16_t signed_int = 16;   // Int (64 bit, deprecated)
inline constexpr uint16_t integer = 32;      // Integer (arbitrary precision)
inline constexpr uint16_t sel = 64;
inline constexpr uint16_t dictionary = 128;
inline constexpr uint16_t unknown = 256;     // Could be anything, so passes every check.

inline constexpr uint16_t numeric = unsigned_int | signed_int | integer;
inline constexpr uint16_t condition = unsigned_int | integer; // What `if` accepts.

constexpr void append_names(text_buffer& out, uint16_t mask)
{
    struct entry { uint16_t kind; std::string_view name; };
    constexpr entry names[] = {
        { object, "Object" }, { type, "Type" }, { string, "String" }, { unsigned_int, "UInt" },
        { signed_int, "Int" }, { integer, "Integer" }, { sel, "Selector" }, { dictionary, "Dictionary" },
        { unknown, "unknown" },
    };
    bool first = true;
    for (const entry& e : names) {
        if (!(mask & e.kind))
            continue;
        if (!first)
            out.append(" or ");
        out.append(e.name);
        first = false;
    }
    if (first)
        out.append("nothing");
}

} // namespace kinds

////////////////////////////////////////////////////////////////////////////////
// Disassembler.
////////////////////////////////////////////////////////////////////////////////

namespace detail {

constexpr uint64_t read_uleb(const byte_buffer& code, size_t& position, size_t end, bool& ok)
{
    uint64_t result = 0;
    unsigned shift = 0;
    while (true) {
        if (position >= end || shift > 63) {
            ok = false;
            return 0;
        }
        uint8_t byte = code.data[position++];
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        shift += 7;
        if (!(byte & 0x80))
            return result;
    }
}

constexpr int64_t read_sleb(const byte_buffer& code, size_t& position, size_t end, bool& ok)
{
    uint64_t result = 0;
    unsigned shift = 0;
    while (true) {
        if (position >= end || shift > 63) {
            ok = false;
            return 0;
        }
        uint8_t byte = code.data[position++];
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        shift += 7;
        if (!(byte & 0x80)) {
            if (shift < 64 && (byte & 0x40))
                result |= ~static_cast<uint64_t>(0) << shift;
            return static_cast<int64_t>(result);
        }
    }
}

constexpr std::string_view mnemonic(opcode code)
{
    for (const auto& entry : opcode_table) {
        if (entry.value == code && !entry.name.empty())
            return entry.name;
    }
    return { };
}

constexpr std::string_view selector_name(uint8_t value)
{
    for (const auto& entry : selector_table) {
        if (static_cast<uint8_t>(entry.value) == value)
            return entry.name;
    }
    return { };
}

constexpr std::string_view signature_name(uint8_t value)
{
    for (const auto& entry : signature_table) {
        if (static_cast<uint8_t>(entry.value) == value)
            return entry.name;
    }
    return { };
}

constexpr void append_bytes(text_buffer& out, const byte_buffer& code, size_t start, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        out.append(static_cast<char>(code.data[start + i]));
}

constexpr void append_quoted(text_buffer& out, const byte_buffer& code, size_t start, size_t length)
{
    out.append('"');
    for (size_t i = 0; i < length && start + i < code.size; ++i) {
        uint8_t c = code.data[start + i];
        if (c == '"' || c == '\\') {
            out.append('\\');
            out.append(static_cast<char>(c));
        } else if (c >= 32 && c < 127)
            out.append(static_cast<char>(c));
        else {
            out.append("\\x");
            out.append_hex(c);
        }
    }
    out.append('"');
}

// Writes `code[begin, end)` as assembly, one block per indented group:
//
//   2u pick 0u = {
//       1
//   } {
//       2
//   } ifelse
//
// If `mark` is an offset in the program, a caret line points at the
// instruction that starts there.
constexpr void disassemble(text_buffer& out, const byte_buffer& code, size_t begin, size_t end, size_t base_indent, size_t mark, std::string_view mark_text)
{
    size_t block_ends[64] { };
    size_t block_count = 0;
    size_t indent = base_indent;
    bool at_line_start = true;
    size_t column = 0; // Characters on the current line.
    size_t mark_column = 0;
    bool mark_pending = false;

    auto newline = [&] {
        if (mark_pending) {
            out.append('\n');
            out.spaces(mark_column);
            out.append("^ ");
            out.append(mark_text);
            mark_pending = false;
        }
        out.append('\n');
        at_line_start = true;
        column = 0;
    };
    auto start_token = [&](size_t at, size_t width) {
        if (at_line_start) {
            out.spaces(indent * 4);
            column = indent * 4;
            at_line_start = false;
        } else {
            out.append(' ');
            ++column;
        }
        if (at == mark) {
            mark_column = column;
            mark_pending = true;
        }
        column += width;
    };
    auto close_blocks = [&](size_t position) {
        while (block_count && block_ends[block_count - 1] <= position) {
            --block_count;
            --indent;
            if (!at_line_start)
                newline();
            out.spaces(indent * 4);
            out.append('}');
            at_line_start = false;
            column = indent * 4 + 1;
        }
    };

    size_t position = begin;
    bool ok = true;
    while (position < end) {
        size_t at = position;
        uint8_t byte = code.data[position++];
        text_buffer token;
        bool is_open = false;
        switch (static_cast<opcode>(byte)) {
        case opcode::begin: {
            size_t length = static_cast<size_t>(read_uleb(code, position, end, ok));
            if (!ok || position + length > end) {
                out.append(" <bad block>");
                return;
            }
            is_open = true;
            if (block_count < 64)
                block_ends[block_count++] = position + length;
            token.append('{');
            break;
        }
        case opcode::lit_uint:
            token.append_number(read_uleb(code, position, end, ok));
            token.append('u');
            break;
        case opcode::lit_int:
        case opcode::lit_integer:
            token.append_signed(read_sleb(code, position, end, ok));
            if (static_cast<opcode>(byte) == opcode::lit_int)
                token.append("i"); // The deprecated Int literal.
            break;
        case opcode::lit_string: {
            size_t length = static_cast<size_t>(read_uleb(code, position, end, ok));
            if (!ok || position + length > end) {
                out.append(" <bad string>");
                return;
            }
            append_quoted(token, code, position, length);
            position += length;
            break;
        }
        case opcode::lit_selector: {
            if (position >= end) {
                out.append(" <bad selector>");
                return;
            }
            uint8_t value = code.data[position++];
            std::string_view name = selector_name(value);
            token.append('@');
            if (name.empty()) {
                token.append("0x");
                token.append_hex(value);
            } else
                token.append(name);
            break;
        }
        default: {
            std::string_view name = mnemonic(static_cast<opcode>(byte));
            if (name.empty()) {
                token.append("<opcode 0x");
                token.append_hex(byte);
                token.append('>');
            } else
                token.append(name);
            break;
        }
        }
        if (!ok) {
            out.append(" <truncated>");
            return;
        }

        if (is_open) {
            // `{` stays on the line of what precedes it.
            start_token(at, 1);
            out.append('{');
            ++indent;
            newline();
        } else {
            start_token(at, token.size());
            out.append(token.view());
        }
        close_blocks(position);
    }
    if (!at_line_start || mark_pending)
        newline();
}

} // namespace detail

////////////////////////////////////////////////////////////////////////////////
// Verifier.
////////////////////////////////////////////////////////////////////////////////

// What can be wrong with a program, checked without running it.
enum class problem_kind : uint8_t {
    none,
    unknown_opcode,
    truncated_program,         // A literal or block runs past the end.
    stack_underflow,           // Pops a value that is not there.
    stack_too_deep,
    type_mismatch,             // Wrong type of value for the instruction.
    operand_types_differ,      // Arithmetic or comparison between, say, UInt and Integer.
    pick_out_of_range,         // `pick` of a slot that does not exist.
    missing_block,             // `if` or `ifelse` without `{ }` blocks before it.
    branches_differ_in_depth,  // The paths through an if leave different numbers of values.
    selector_not_constant,     // `call` where the selector is not a literal.
    selector_not_implemented,  // LLDB's interpreter does not implement it.
    format_not_literal,        // @fmt whose format is not a string literal.
    fmt_formats_integer_wrongly, // @fmt of a UInt or Int prints a pointer (LLDB bug).
    empty_result,              // The program leaves nothing to return.
    wrong_result_type,         // The result is not what this signature needs.
    nested_conditional_not_last, // An LLDB bug would drop the code after it.
};

constexpr std::string_view problem_text(problem_kind problem)
{
    switch (problem) {
    case problem_kind::none: return "no problem";
    case problem_kind::unknown_opcode: return "unknown opcode";
    case problem_kind::truncated_program: return "a literal or block runs past the end of the program";
    case problem_kind::stack_underflow: return "pops a value that is not on the stack";
    case problem_kind::stack_too_deep: return "the stack gets too deep to verify";
    case problem_kind::type_mismatch: return "type mismatch";
    case problem_kind::operand_types_differ: return "operands have different types (UInt and Integer do not mix; convert one)";
    case problem_kind::pick_out_of_range: return "`pick` of a slot that does not exist";
    case problem_kind::missing_block: return "`if`/`ifelse` without the blocks it runs";
    case problem_kind::branches_differ_in_depth: return "the two paths through this conditional leave different numbers of values";
    case problem_kind::selector_not_constant: return "`call` needs a selector literal right before it";
    case problem_kind::selector_not_implemented: return "LLDB's interpreter does not implement this selector";
    case problem_kind::format_not_literal: return "@fmt needs a string literal as its format";
    case problem_kind::fmt_formats_integer_wrongly: return "@fmt prints a bogus pointer for a UInt or Int argument (LLDB bug; widen to Integer or use @get_value, or define LLDBFMT_FMT_FORMATS_INTEGERS_CORRECTLY)";
    case problem_kind::empty_result: return "the program leaves no value to return";
    case problem_kind::wrong_result_type: return "the program returns the wrong type for this signature";
    case problem_kind::nested_conditional_not_last: return "a conditional inside another conditional's block loses the code after the outer one (LLDB bug; define LLDBFMT_ALLOW_NESTED_CONDITIONALS for an LLDB that fixes it)";
    }
    return { };
}

// The types of the values a program starts with. When `open`, the stack may
// hold more values than the `count` listed (a getter whose attributes the
// record does not describe).
struct slot_types {
    std::array<uint16_t, 24> kinds { };
    size_t count { 0 };
    bool open { true };
};

namespace detail {

struct slot {
    uint16_t kinds { kinds::unknown };
    bool constant { false };
    uint64_t value { 0 };
    size_t string_start { 0 };
    size_t string_length { 0 };
};

struct stack_state {
    std::array<slot, 96> stack { };
    size_t depth { 0 };
    bool open { false };
    bool terminated { false };
};

struct code_block {
    size_t begin;
    size_t end;
};

struct selector_signature {
    selector sel;
    uint8_t argument_count;
    std::array<uint16_t, 4> arguments; // bottom to top
    uint16_t result;
};

// What each selector the interpreter implements takes and returns
// (FormatterBytecode.cpp). Not listed: @fmt, which is special, and the ones
// that are not implemented.
inline constexpr selector_signature selector_signatures[] = {
    { selector::summary, 1, { kinds::object }, kinds::string },
    { selector::get_num_children, 1, { kinds::object }, kinds::unsigned_int },
    { selector::get_child_at_index, 2, { kinds::object, kinds::unsigned_int }, kinds::object },
    { selector::get_child_with_name, 2, { kinds::object, kinds::string }, kinds::object },
    { selector::get_child_index, 2, { kinds::object, kinds::string }, kinds::unsigned_int },
    { selector::get_parent, 1, { kinds::object }, kinds::object },
    { selector::get_type, 1, { kinds::object }, kinds::type },
    { selector::get_template_argument_type, 2, { kinds::type, kinds::unsigned_int }, kinds::type },
    { selector::cast, 2, { kinds::object, kinds::type }, kinds::object },
    { selector::get_synthetic_value, 1, { kinds::object }, kinds::object },
    { selector::get_non_synthetic_value, 1, { kinds::object }, kinds::object },
    { selector::get_value, 1, { kinds::object }, kinds::string },
    { selector::get_value_as_unsigned, 1, { kinds::object }, kinds::unsigned_int },
    { selector::get_value_as_signed, 1, { kinds::object }, kinds::signed_int },
    { selector::get_value_as_address, 1, { kinds::object }, kinds::unsigned_int },
    { selector::clone, 2, { kinds::object, kinds::string }, kinds::object },
    { selector::get_pointee_type, 1, { kinds::type }, kinds::type },
    { selector::get_byte_size, 1, { kinds::type }, kinds::integer },
    { selector::create_child_at_offset, 4, { kinds::object, kinds::string, kinds::integer, kinds::type }, kinds::object },
    { selector::strlen, 1, { kinds::string }, kinds::unsigned_int },
};

// The number of arguments @fmt takes: one past the highest {N} in the format.
constexpr size_t format_argument_count(const byte_buffer& code, size_t start, size_t length)
{
    size_t count = 0;
    for (size_t i = 0; i < length; ++i) {
        char c = static_cast<char>(code.data[start + i]);
        if (c != '{')
            continue;
        if (i + 1 < length && code.data[start + i + 1] == '{') {
            ++i; // `{{` is a literal brace.
            continue;
        }
        size_t index = 0;
        bool digits = false;
        size_t j = i + 1;
        while (j < length && code.data[start + j] >= '0' && code.data[start + j] <= '9') {
            index = index * 10 + (code.data[start + j] - '0');
            digits = true;
            ++j;
        }
        if (digits && index + 1 > count)
            count = index + 1;
        i = j;
    }
    return count;
}

struct verifier {
    const byte_buffer& code;
    signature sig;
    problem_kind problem { problem_kind::none };
    size_t problem_offset { 0 };
    uint16_t expected { 0 };
    uint16_t actual { 0 };
    unsigned enclosing_not_last { 0 }; // Running blocks of conditionals that code follows.

    constexpr bool failed() const
    {
        return problem != problem_kind::none;
    }

    constexpr void fail(problem_kind p, size_t offset, uint16_t expected_kinds = 0, uint16_t actual_kinds = 0)
    {
        if (failed())
            return;
        problem = p;
        problem_offset = offset;
        expected = expected_kinds;
        actual = actual_kinds;
    }

    constexpr void push(stack_state& state, slot slot, size_t at)
    {
        if (state.depth == state.stack.size()) {
            fail(problem_kind::stack_too_deep, at);
            return;
        }
        state.stack[state.depth++] = slot;
    }

    constexpr void push(stack_state& state, uint16_t kinds, size_t at)
    {
        slot slot;
        slot.kinds = kinds;
        push(state, slot, at);
    }

    constexpr slot pop(stack_state& state, size_t at)
    {
        if (state.depth == 0) {
            if (!state.open)
                fail(problem_kind::stack_underflow, at);
            return slot { };
        }
        return state.stack[--state.depth];
    }

    // Is a value of these kinds acceptable where `allowed` is wanted?
    constexpr bool expect(const slot& slot, uint16_t allowed, size_t at)
    {
        if ((slot.kinds & kinds::unknown) || !(slot.kinds & ~allowed))
            return true;
        fail(problem_kind::type_mismatch, at, allowed, slot.kinds);
        return false;
    }

    static constexpr stack_state join(const stack_state& a, const stack_state& b, verifier& v, size_t at)
    {
        if (a.terminated)
            return b;
        if (b.terminated)
            return a;
        stack_state result = a;
        if (a.depth != b.depth && !(a.open && b.open)) {
            v.fail(problem_kind::branches_differ_in_depth, at);
            return result;
        }
        for (size_t i = 0; i < result.depth && i < b.depth; ++i) {
            result.stack[i].kinds |= b.stack[i].kinds;
            result.stack[i].constant = false;
        }
        result.open = a.open || b.open;
        return result;
    }

    constexpr void check_result(const stack_state& state, size_t at)
    {
        if (state.depth == 0) {
            if (!state.open)
                fail(problem_kind::empty_result, at);
            return;
        }
        uint16_t allowed;
        switch (sig) {
        case signature::summary:
        case signature::get_value:
            allowed = kinds::string;
            break;
        case signature::get_num_children:
        case signature::get_child_index:
            allowed = kinds::numeric;
            break;
        case signature::get_child_at_index:
            allowed = kinds::object;
            break;
        default:
            return;
        }
        const slot& top = state.stack[state.depth - 1];
        if ((top.kinds & kinds::unknown) || !(top.kinds & ~allowed))
            return;
        fail(problem_kind::wrong_result_type, at, allowed, top.kinds);
    }

    constexpr void call(stack_state& state, size_t at)
    {
        slot selector_slot = pop(state, at);
        if (failed())
            return;
        if (!selector_slot.constant || selector_slot.kinds != kinds::sel) {
            if (!(selector_slot.kinds & kinds::unknown))
                expect(selector_slot, kinds::sel, at);
            fail(problem_kind::selector_not_constant, at);
            return;
        }
        selector sel = static_cast<selector>(selector_slot.value);

        if (sel == selector::fmt) {
            slot format = pop(state, at);
            if (!expect(format, kinds::string, at))
                return;
            if (!format.constant) {
                fail(problem_kind::format_not_literal, at);
                return;
            }
            size_t count = format_argument_count(code, format.string_start, format.string_length);
            if (state.depth < count && !state.open) {
                fail(problem_kind::stack_underflow, at);
                return;
            }
            for (size_t i = 0; i < count && i < state.depth; ++i) {
                const slot& argument = state.stack[state.depth - 1 - i];
                bool integer_like = argument.kinds & (kinds::unsigned_int | kinds::signed_int);
                if (integer_like && !(argument.kinds & kinds::unknown) && !LLDBFMT_FMT_FORMATS_INTEGERS_CORRECTLY) {
                    fail(problem_kind::fmt_formats_integer_wrongly, at, kinds::string | kinds::integer, argument.kinds);
                    return;
                }
            }
            if (LLDBFMT_FMT_CONSUMES_ARGUMENTS) {
                for (size_t i = 0; i < count; ++i)
                    pop(state, at);
            }
            push(state, kinds::string, at);
            return;
        }

        for (const selector_signature& entry : selector_signatures) {
            if (entry.sel != sel)
                continue;
            for (size_t i = entry.argument_count; i-- > 0;) {
                slot argument = pop(state, at);
                if (!expect(argument, entry.arguments[i], at))
                    return;
            }
            push(state, entry.result, at);
            return;
        }
        fail(problem_kind::selector_not_implemented, at);
    }

    constexpr void binary(stack_state& state, size_t at, uint16_t allowed)
    {
        slot y = pop(state, at);
        slot x = pop(state, at);
        if (failed())
            return;
        if (!expect(y, allowed, at) || !expect(x, allowed, at))
            return;
        bool known = !(x.kinds & kinds::unknown) && !(y.kinds & kinds::unknown);
        if (known && x.kinds != y.kinds) {
            fail(problem_kind::operand_types_differ, at, x.kinds, y.kinds);
            return;
        }
        push(state, (x.kinds & kinds::unknown) ? y.kinds : x.kinds, at);
    }

    // Runs code[begin, end) abstractly. `executing` counts the blocks being run.
    constexpr void run(stack_state& state, size_t begin, size_t end, unsigned executing)
    {
        code_block control[32] { };
        size_t control_count = 0;
        size_t position = begin;
        size_t flagged_at = ~static_cast<size_t>(0);

        auto pop_block = [&](size_t at) -> code_block {
            if (!control_count) {
                fail(problem_kind::missing_block, at);
                return { 0, 0 };
            }
            return control[--control_count];
        };

        while (position < end && !failed() && !state.terminated) {
            size_t at = position;
            uint8_t byte = code.data[position++];
            bool ok = true;
            switch (static_cast<opcode>(byte)) {
            case opcode::dup: {
                if (state.depth == 0) {
                    if (!state.open)
                        fail(problem_kind::stack_underflow, at);
                    push(state, kinds::unknown, at);
                } else
                    push(state, state.stack[state.depth - 1], at);
                break;
            }
            case opcode::drop:
                pop(state, at);
                break;
            case opcode::over: {
                if (state.depth < 2) {
                    if (!state.open)
                        fail(problem_kind::stack_underflow, at);
                    push(state, kinds::unknown, at);
                } else
                    push(state, state.stack[state.depth - 2], at);
                break;
            }
            case opcode::swap: {
                slot top = pop(state, at);
                slot below = pop(state, at);
                push(state, top, at);
                push(state, below, at);
                break;
            }
            case opcode::rot: {
                slot z = pop(state, at);
                slot y = pop(state, at);
                slot x = pop(state, at);
                push(state, z, at);
                push(state, x, at);
                push(state, y, at);
                break;
            }
            case opcode::pick: {
                slot index = pop(state, at);
                if (!expect(index, kinds::unsigned_int, at))
                    break;
                // With an unknown starting stack an absolute index cannot be
                // resolved against what this program has pushed.
                if (!index.constant || state.open) {
                    push(state, kinds::unknown, at);
                    break;
                }
                if (index.value < state.depth) {
                    slot copy = state.stack[index.value];
                    copy.constant = false;
                    push(state, copy, at);
                } else if (state.open)
                    push(state, kinds::unknown, at);
                else
                    fail(problem_kind::pick_out_of_range, at, 0, 0);
                break;
            }
            case opcode::begin: {
                size_t length = static_cast<size_t>(read_uleb(code, position, end, ok));
                if (!ok || position + length > end || control_count == 32) {
                    fail(problem_kind::truncated_program, at);
                    break;
                }
                control[control_count++] = { position, position + length };
                position += length;
                break;
            }
            case opcode::if_:
            case opcode::if_else: {
                bool is_else = static_cast<opcode>(byte) == opcode::if_else;
                slot condition = pop(state, at);
                if (!expect(condition, kinds::condition, at))
                    break;
                code_block second = pop_block(at);
                code_block first = is_else ? pop_block(at) : code_block { 0, 0 };
                if (failed())
                    break;
                // `if` runs its one block (the one popped first). `ifelse` has
                // the "then" block pushed first and the "else" block second.
                code_block then_block = is_else ? first : second;

                // When a block finishes with two or more continuations below
                // it, LLDB overwrites the one under the one it resumes with an
                // empty remainder (resuming saves a return address, which is
                // only right when `if` or `ifelse` enters a block). So a
                // conditional that runs inside another's block loses the code
                // after every enclosing conditional: harmless only if nothing
                // follows them.
                // (If the block that holds the nested conditional ends in
                // `return`, nothing below it is ever resumed, so it is fine.)
                bool last = position == end;
                if (executing >= 1 && enclosing_not_last > 0 && flagged_at == ~static_cast<size_t>(0))
                    flagged_at = at;
                if (!last)
                    ++enclosing_not_last;

                stack_state then_state = state;
                run(then_state, then_block.begin, then_block.end, executing + 1);
                stack_state other_state = state;
                if (is_else)
                    run(other_state, second.begin, second.end, executing + 1);
                state = join(then_state, other_state, *this, at);

                if (!last)
                    --enclosing_not_last;
                break;
            }
            case opcode::return_:
                check_result(state, at);
                state.terminated = true;
                break;
            case opcode::lit_uint: {
                slot slot;
                slot.kinds = kinds::unsigned_int;
                slot.constant = true;
                slot.value = read_uleb(code, position, end, ok);
                push(state, slot, at);
                break;
            }
            case opcode::lit_int: {
                slot slot;
                slot.kinds = kinds::signed_int;
                slot.value = static_cast<uint64_t>(read_sleb(code, position, end, ok));
                push(state, slot, at);
                break;
            }
            case opcode::lit_integer: {
                slot slot;
                slot.kinds = kinds::integer;
                slot.value = static_cast<uint64_t>(read_sleb(code, position, end, ok));
                push(state, slot, at);
                break;
            }
            case opcode::lit_string: {
                size_t length = static_cast<size_t>(read_uleb(code, position, end, ok));
                if (!ok || position + length > end) {
                    ok = false;
                    break;
                }
                slot slot;
                slot.kinds = kinds::string;
                slot.constant = true;
                slot.string_start = position;
                slot.string_length = length;
                position += length;
                push(state, slot, at);
                break;
            }
            case opcode::lit_selector: {
                if (position >= end) {
                    ok = false;
                    break;
                }
                slot slot;
                slot.kinds = kinds::sel;
                slot.constant = true;
                slot.value = code.data[position++];
                push(state, slot, at);
                break;
            }
            case opcode::lit_null:
                push(state, kinds::object, at);
                break;
            case opcode::as_int: {
                slot x = pop(state, at);
                if (expect(x, kinds::unsigned_int, at))
                    push(state, kinds::signed_int, at);
                break;
            }
            case opcode::as_uint: {
                slot x = pop(state, at);
                if (expect(x, kinds::signed_int, at))
                    push(state, kinds::unsigned_int, at);
                break;
            }
            case opcode::is_null: {
                slot x = pop(state, at);
                if (expect(x, kinds::object, at))
                    push(state, kinds::unsigned_int, at);
                break;
            }
            case opcode::plus:
            case opcode::minus:
            case opcode::mul:
            case opcode::div:
            case opcode::mod:
            case opcode::and_:
            case opcode::or_:
            case opcode::xor_:
                binary(state, at, kinds::numeric);
                break;
            case opcode::eq:
            case opcode::neq:
            case opcode::lt:
            case opcode::gt:
            case opcode::le:
            case opcode::ge:
                binary(state, at, kinds::numeric);
                break;
            case opcode::shl:
            case opcode::shr: {
                slot count = pop(state, at);
                slot x = pop(state, at);
                if (expect(count, kinds::unsigned_int, at) && expect(x, kinds::numeric, at))
                    push(state, x.kinds, at);
                break;
            }
            case opcode::not_: {
                slot x = pop(state, at);
                if (expect(x, kinds::unsigned_int | kinds::integer, at))
                    push(state, x.kinds, at);
                break;
            }
            case opcode::call:
                call(state, at);
                break;
            case opcode::dict:
                push(state, kinds::dictionary, at);
                break;
            case opcode::dict_set: {
                pop(state, at);
                slot key = pop(state, at);
                slot dictionary = pop(state, at);
                expect(key, kinds::string, at);
                expect(dictionary, kinds::dictionary, at);
                break;
            }
            case opcode::dict_get: {
                slot key = pop(state, at);
                slot dictionary = pop(state, at);
                if (expect(key, kinds::string, at) && expect(dictionary, kinds::dictionary, at))
                    push(state, kinds::unknown, at);
                break;
            }
            case opcode::dict_has: {
                slot key = pop(state, at);
                slot dictionary = pop(state, at);
                if (expect(key, kinds::string, at) && expect(dictionary, kinds::dictionary, at))
                    push(state, kinds::integer, at);
                break;
            }
            default:
                fail(problem_kind::unknown_opcode, at);
                break;
            }
            if (!ok)
                fail(problem_kind::truncated_program, at);
        }
        if (flagged_at != ~static_cast<size_t>(0) && !state.terminated && !LLDBFMT_ALLOW_NESTED_CONDITIONALS)
            fail(problem_kind::nested_conditional_not_last, flagged_at);
    }
};

} // namespace detail

struct verify_result {
    problem_kind problem { problem_kind::none };
    size_t offset { 0 };
    uint16_t expected { 0 };
    uint16_t actual { 0 };
};

// Runs `program` abstractly as the given signature's program.
constexpr verify_result verify(const byte_buffer& program, signature sig, const slot_types& slots)
{
    detail::verifier verifier { program, sig };
    detail::stack_state state;
    state.open = slots.open;
    for (size_t i = 0; i < slots.count; ++i) {
        detail::slot slot;
        slot.kinds = slots.kinds[i];
        state.stack[state.depth++] = slot;
    }
    verifier.run(state, 0, program.size, 0);
    if (!verifier.failed() && !state.terminated)
        verifier.check_result(state, program.size);
    return { verifier.problem, verifier.problem_offset, verifier.expected, verifier.actual };
}

////////////////////////////////////////////////////////////////////////////////
// Looking inside a serialized record.
////////////////////////////////////////////////////////////////////////////////

namespace detail {

struct record_view {
    bool valid { false };
    size_t type_name_start { 0 };
    size_t type_name_length { 0 };
    uint64_t flags { 0 };
    size_t program_count { 0 };
    std::array<uint8_t, signature_count> signatures { };
    std::array<size_t, signature_count> begins { };
    std::array<size_t, signature_count> ends { };
};

constexpr record_view parse_record(const byte_buffer& record)
{
    record_view view;
    size_t position = 0;
    bool ok = true;
    uint64_t version = read_uleb(record, position, record.size, ok);
    uint64_t length = read_uleb(record, position, record.size, ok);
    if (!ok || version != record_version || position + length > record.size)
        return view;
    size_t end = position + static_cast<size_t>(length);
    size_t name_length = static_cast<size_t>(read_uleb(record, position, end, ok));
    view.type_name_start = position;
    view.type_name_length = name_length;
    position += name_length;
    view.flags = read_uleb(record, position, end, ok);
    while (ok && position < end && view.program_count < signature_count) {
        uint8_t sig = record.data[position++];
        size_t size = static_cast<size_t>(read_uleb(record, position, end, ok));
        if (!ok || position + size > end)
            return view;
        view.signatures[view.program_count] = sig;
        view.begins[view.program_count] = position;
        view.ends[view.program_count] = position + size;
        ++view.program_count;
        position += size;
    }
    view.valid = ok;
    return view;
}

constexpr void append_program(text_buffer& out, const byte_buffer& record, const record_view& view, size_t index, size_t mark, std::string_view mark_text)
{
    out.append("  @");
    out.append(signature_name(view.signatures[index]));
    out.append(" (");
    out.append_number(view.ends[index] - view.begins[index]);
    out.append(" bytes):\n");
    disassemble(out, record, view.begins[index], view.ends[index], 1, mark, mark_text);
}

} // namespace detail

// The disassembly of a whole record.
constexpr text_buffer disassemble(const byte_buffer& record)
{
    text_buffer out;
    detail::record_view view = detail::parse_record(record);
    if (!view.valid) {
        out.append("<not a formatter record>\n");
        return out;
    }
    out.append("lldbfmt record \"");
    detail::append_bytes(out, record, view.type_name_start, view.type_name_length);
    out.append("\" (flags ");
    out.append_number(view.flags);
    out.append(", ");
    out.append_number(record.size);
    out.append(" bytes)\n");
    for (size_t i = 0; i < view.program_count; ++i)
        detail::append_program(out, record, view, i, ~static_cast<size_t>(0), { });
    return out;
}

// The disassembly of a single program, as `assemble` or a DSL method makes it.
constexpr text_buffer disassemble_program(const byte_buffer& program)
{
    text_buffer out;
    detail::disassemble(out, program, 0, program.size, 0, ~static_cast<size_t>(0), { });
    return out;
}

// A verification failure (byte_buffer::problem*) in words, with the program
// disassembled and the instruction pointed to.
constexpr text_buffer diagnose(const byte_buffer& record)
{
    text_buffer out;
    detail::record_view view = detail::parse_record(record);
    out.append("lldbfmt: ");
    if (view.valid) {
        out.append('"');
        detail::append_bytes(out, record, view.type_name_start, view.type_name_length);
        out.append("\": ");
    }
    out.append('@');
    out.append(detail::signature_name(record.problem_signature));
    out.append(" does not verify\n  error: ");
    out.append(problem_text(static_cast<problem_kind>(record.problem)));
    if (record.problem_expected && static_cast<problem_kind>(record.problem) == problem_kind::operand_types_differ) {
        out.append(" (left is ");
        kinds::append_names(out, record.problem_expected);
        out.append(", right is ");
        kinds::append_names(out, record.problem_actual);
        out.append(')');
    } else if (record.problem_expected) {
        out.append(" (expected ");
        kinds::append_names(out, record.problem_expected);
        out.append(", found ");
        kinds::append_names(out, record.problem_actual);
        out.append(')');
    }
    out.append("\n\n");
    if (view.valid) {
        for (size_t i = 0; i < view.program_count; ++i) {
            if (view.signatures[i] != record.problem_signature)
                continue;
            size_t mark = view.begins[i] + record.problem_offset;
            detail::append_program(out, record, view, i, mark, "here");
        }
    }
    return out;
}

} // namespace lldbfmt

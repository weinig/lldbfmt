// Compile-time tests: if this file compiles, the tests pass.
// Expected bytes come from the reference Python assembler and the LLDB docs.

#include "lldbfmt/lldbfmt.h"

#include <initializer_list>

using namespace lldbfmt;
using namespace lldbfmt::dsl;

constexpr bool equals(const byte_buffer& bytes, std::initializer_list<int> expected)
{
    if (bytes.size != expected.size())
        return false;
    size_t i = 0;
    for (int byte : expected) {
        if (bytes.data[i++] != byte)
            return false;
    }
    return true;
}

constexpr bool equals(const byte_buffer& a, const byte_buffer& b)
{
    if (a.size != b.size)
        return false;
    for (size_t i = 0; i < a.size; ++i) {
        if (a.data[i] != b.data[i])
            return false;
    }
    return true;
}

// LLDB's Python tests' "Account" record: 01 15 07 "Account" 00 | 02 02 20 01 | 04 06 02 20 00 23 11 60
constexpr auto accountExpected = std::initializer_list<int> { 0x01, 0x15, 0x07, 'A', 'c', 'c', 'o', 'u', 'n', 't', 0x00,
    0x02, 0x02, 0x20, 0x01, 0x04, 0x06, 0x02, 0x20, 0x00, 0x23, 0x11, 0x60 };

static_assert(equals(record("Account", 0,
    get_num_children(1u),
    get_child_at_index(drop, 0u, selector::get_child_at_index, call)), accountExpected));
static_assert(equals(record_from_assembly("Account", 0,
    "@get_num_children: 1u\n@get_child_at_index: drop 0u @get_child_at_index call"), accountExpected));
// Method order does not matter. Output is in signature order.
static_assert(equals(record("Account", 0,
    get_child_at_index(drop, 0u, selector::get_child_at_index, call),
    get_num_children(1u)), accountExpected));

// Assembler parity with the Python tests.
static_assert(equals(assemble("1u dup"), { 0x20, 0x01, 0x01 }));
static_assert(equals(assemble("\"1u dup\""), { 0x22, 0x06, '1', 'u', ' ', 'd', 'u', 'p' }));
static_assert(equals(assemble("16 < { dup } if"), { 0x24, 0x10, 0x52, 0x10, 0x01, 0x01, 0x11 }));
static_assert(equals(assemble("{ { \" } \" } }"), { 0x10, 0x07, 0x10, 0x05, 0x22, 0x03, ' ', '}', ' ' }));
static_assert(equals(assemble("\"a \\\" b\""), { 0x22, 0x05, 'a', ' ', '"', ' ', 'b' }));
static_assert(equals(assemble("null is_null"), { 0x25, 0x2c }));

// LEB128 encodings: lengths and literals are not limited to one byte.
constexpr byte_buffer lebs()
{
    byte_buffer b;
    b.append_uleb(300); b.append_uleb(0); b.append_uleb(127); b.append_uleb(128);
    b.append_sleb(-1); b.append_sleb(63); b.append_sleb(64); b.append_sleb(-64); b.append_sleb(-65);
    return b;
}
static_assert(equals(lebs(), { 0xac, 0x02, 0x00, 0x7f, 0x80, 0x01, 0x7f, 0x3f, 0xc0, 0x00, 0x40, 0xbf, 0x7f }));
static_assert(equals(assemble("-1 64 0x10 300u"), { 0x24, 0x7f, 0x24, 0xc0, 0x00, 0x24, 0x10, 0x20, 0xac, 0x02 }));

constexpr byte_buffer sequenceBytes(const auto& element)
{
    byte_buffer b;
    element.emit(b);
    return b;
}

// DSL and assembler agree.
constexpr byte_buffer viaDSL(auto... elements) { return method(signature::summary, elements...).bytecode; }
static_assert(equals(viaDSL(16, less, if_then(block(dup))), { 0x24, 0x10, 0x52, 0x10, 0x01, 0x01, 0x11 }));
static_assert(equals(viaDSL(2, 1, 1, plus, mul), assemble("2 1 1 + *")));
static_assert(equals(viaDSL(2, 1, greater, if_then_else(block("yes"), block("no"))), assemble("2 1 > { \"yes\" } { \"no\" } ifelse")));
static_assert(equals(viaDSL(null, is_null, 3u, pick, selector::fmt, call, return_), assemble("null is_null 3u pick @fmt call return")));

// UInt -> Integer conversion, unrolled per bit.
static_assert(equals(sequenceBytes(uint_to_integer<2>()),
    assemble("0 swap dup 0u >> 1u & { swap 1 0u << + swap } if dup 1u >> 1u & { swap 1 1u << + swap } if drop")));

// A block longer than 127 bytes needs a two-byte length (the Python assembler
// would emit a single wrong byte here).
constexpr auto bigText = [] {
    std::array<char, 1 + 200 * 4 + 2> text { };
    size_t n = 0;
    text[n++] = '{';
    for (int i = 0; i < 200; ++i) {
        text[n++] = ' ';
        text[n++] = 'd'; text[n++] = 'u'; text[n++] = 'p';
    }
    text[n++] = ' ';
    text[n++] = '}';
    return text;
}();
constexpr byte_buffer bigBlock = assemble(std::string_view(bigText.data(), bigText.size()));
static_assert(bigBlock.size == 1 + 2 + 200);
static_assert(bigBlock.data[0] == 0x10 && bigBlock.data[1] == 0xc8 && bigBlock.data[2] == 0x01 && bigBlock.data[3] == 0x01);

// Section labels, whitespace and flags.
static_assert(equals(record_from_assembly("T", 1 << 10, "@summary:   return  "), { 0x01, 0x07, 0x01, 'T', 0x80, 0x08, 0x00, 0x01, 0x13 }));
static_assert(equals(record("^T<.+>$", 0, summary(return_)), { 0x01, 0x0c, 0x07, '^', 'T', '<', '.', '+', '>', '$', 0x00, 0x00, 0x01, 0x13 }));

int main() { }

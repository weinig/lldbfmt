// Compile-time tests for lldbfmt/script.h: each expression must lower to
// exactly the bytecode the assembler gives for the equivalent stack program.

#include "lldbfmt/script.h"

using namespace lldbfmt;
using namespace lldbfmt::script;

constexpr byte_buffer bytesOf(const auto& node)
{
    byte_buffer bytes;
    node.emit(bytes);
    return bytes;
}

constexpr bool same(const byte_buffer& a, const byte_buffer& b)
{
    if (a.size != b.size)
        return false;
    for (size_t i = 0; i < a.size; ++i) {
        if (a.data[i] != b.data[i])
            return false;
    }
    return true;
}

// Attributes as a synthetic getter sees them: valobj, then a UInt and an Integer.
constexpr local<object_tag, 0> valobj;
constexpr local<uint_tag, 2> size;
constexpr local<integer_tag, 3> dataSize;
constexpr local<uint_tag, 4> at;

// SBValue calls become selector calls on the receiver.
static_assert(same(bytesOf(valobj.get_child_member_with_name("m_x").get_value_as_signed()),
    assemble("0u pick \"m_x\" @get_child_with_name call @get_value_as_signed call")));
static_assert(same(bytesOf(valobj.get_type().get_pointee_type().get_byte_size()),
    assemble("0u pick @get_type call @get_pointee_type call @get_byte_size call")));

// Integer literals take the other operand's type.
static_assert(same(bytesOf(size + 3), assemble("2u pick 3u +")));
static_assert(same(bytesOf(3 + size), assemble("3u 2u pick +")));
static_assert(same(bytesOf(dataSize + 3), assemble("3u pick 3 +")));

// Comparing Ints reinterprets the result so that `if` accepts it.
static_assert(bytesOf(valobj.get_value_as_signed() < 0).data[bytesOf(valobj.get_value_as_signed() < 0).size - 1] == 0x2b);

// UInt combined with Integer widens the UInt on the spot.
static_assert(same(bytesOf(at * dataSize), [] {
    byte_buffer b;
    at.emit(b);
    dsl::uint_to_integer<LLDBFMT_PROMOTION_BITS>().emit(b);
    dataSize.emit(b);
    b.append(opcode::mul);
    return b;
}()));

// Shifts take a UInt count and keep the left type.
static_assert(same(bytesOf(dataSize << 2), assemble("3u pick 2u <<")));
static_assert(same(bytesOf(size >> 1), assemble("2u pick 1u >>")));

// Conditionals and chains.
static_assert(same(bytesOf(if_(size == 0, 1, 2)), assemble("2u pick 0u = { 1 } { 2 } ifelse")));
static_assert(same(bytesOf(if_(size == 0, "a", size == 1, "b", "c")),
    assemble("2u pick 0u = { \"a\" } { 2u pick 1u = { \"b\" } { \"c\" } ifelse } ifelse")));
static_assert(same(bytesOf(if_(size == 0, valobj, null)), assemble("2u pick 0u = { 0u pick } { null } ifelse")));

// Logical operators short-circuit and are built from flat `dup { drop rhs } if`
// steps rather than nested conditionals (see LLDBFMT_ALLOW_NESTED_CONDITIONALS).
static_assert(same(bytesOf(size == 1 && at == 2), assemble("2u pick 1u = dup { drop 4u pick 2u = } if")));
static_assert(same(bytesOf(size == 1 || at == 2), assemble("2u pick 1u = dup { } { drop 4u pick 2u = } ifelse")));
static_assert(same(bytesOf(size == 1 && at == 2 && size == 3),
    assemble("2u pick 1u = dup { drop 4u pick 2u = } if dup { drop 2u pick 3u = } if")));
static_assert(same(bytesOf(if_(size == 1 && at == 2, 1, 2)),
    assemble("2u pick 1u = dup { drop 4u pick 2u = } if { 1 } { 2 } ifelse")));
static_assert(same(bytesOf(!size), assemble("2u pick { 0u } { 1u } ifelse")));

// Format: Python specifiers, escaped braces, and the fmt workarounds.
static_assert(same(bytesOf(format("{ x = %s }", valobj.get_child_member_with_name("m_x").get_value_as_signed())),
    assemble("0u pick \"m_x\" @get_child_with_name call @get_value call \"{{ x = {0} }\" @fmt call swap drop")));
static_assert(same(bytesOf(format("100%% %s", "x")), assemble("\"x\" \"100% {0}\" @fmt call swap drop")));
static_assert(same(bytesOf(format("[%d]", at)), [] {
    byte_buffer b;
    at.emit(b);
    dsl::uint_to_integer<LLDBFMT_PROMOTION_BITS>().emit(b);
    b.append(assemble("\"[{0}]\" @fmt call swap drop"));
    return b;
}()));

// Lookup builds a dictionary, and the values share a type.
static_assert(same(bytesOf(lookup(valobj.get_value(), "a", size, "b", size + 1)),
    assemble("dict dup \"a\" 2u pick dict_set dup \"b\" 2u pick 1u + dict_set 0u pick @get_value call dict_get")));

// Records: summary, and a synthetic provider with state.
constexpr byte_buffer pointSummary = summary("P", [](auto valobj) {
    return format("(%s, %s)", valobj.get_child_member_with_name("a").get_value_as_signed(), valobj.get_child_member_with_name("b").get_value_as_signed());
});
static_assert(same(pointSummary, record_from_assembly("P", type_options::cascade,
    "@summary: 0u pick \"a\" @get_child_with_name call @get_value call "
    "0u pick \"b\" @get_child_with_name call @get_value call \"({0}, {1})\" @fmt call swap drop swap drop")));

constexpr byte_buffer children = synthetic("T", 0,
    update([]([[maybe_unused]] auto valobj) {
        auto size = valobj.get_child_member_with_name("m_size").get_value_as_unsigned();
        return state(valobj.get_child_member_with_name("m_buffer"), size);
    }),
    num_children([]([[maybe_unused]] auto valobj, [[maybe_unused]] auto buffer, auto size) { return size + 1; }),
    child_at_index([]([[maybe_unused]] auto valobj, [[maybe_unused]] auto buffer, auto size, auto at) { return if_(at < size, buffer, null); }));
static_assert(same(children, record_from_assembly("T", 0,
    "@update: 0u pick \"m_buffer\" @get_child_with_name call "
    "0u pick \"m_size\" @get_child_with_name call @get_value_as_unsigned call 0u "
    "@get_num_children: 2u pick 1u + "
    "@get_child_at_index: 3u pick 2u pick < { 1u pick } { null } ifelse")));

// The same record again with an @init that supplies the buffer and an @update that refreshes the size.
constexpr byte_buffer split = synthetic("T", 0,
    init([]([[maybe_unused]] auto valobj) { return state(valobj.get_child_member_with_name("m_buffer")); }),
    update([]([[maybe_unused]] auto valobj, [[maybe_unused]] auto buffer) { return state(valobj.get_child_member_with_name("m_size").get_value_as_unsigned()); }),
    num_children([]([[maybe_unused]] auto valobj, [[maybe_unused]] auto buffer, auto size) { return size; }));
static_assert(same(split, record_from_assembly("T", 0,
    "@init: 0u pick \"m_buffer\" @get_child_with_name call "
    "@update: 0u pick \"m_size\" @get_child_with_name call @get_value_as_unsigned call 0u "
    "@get_num_children: 2u pick")));

int main() { }

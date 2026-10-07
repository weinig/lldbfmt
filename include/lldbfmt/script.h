// Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// lldbfmt/script.h: a Python-shaped way to write LLDB formatters.
//
// Formatters are written with the same SBValue/SBType calls and operators that
// a Python data formatter uses, and compile to formatter bytecode during
// constant evaluation. Built on lldbfmt/lldbfmt.h, whose DSL is the low-level
// layer: every expression here is also a valid element of that DSL.
//
//   LLDBFMT_SCRIPT_SUMMARY(IntRectSummary, "WebCore::IntRect", [](auto valobj) {
//       auto location = valobj.get_child_member_with_name("m_location");
//       return format("{ x = %s, y = %s }",
//           location.get_child_member_with_name("m_x").get_value_as_signed(),
//           location.get_child_member_with_name("m_y").get_value_as_signed());
//   });
//
// How it works:
//
// * Everything is an expression tree. `valobj.get_child_member_with_name("m_x")` is
//   a node, operators and calls build bigger nodes, and `emit` lowers a tree to
//   stack code. There are no variables: `auto x = expr;` just names a
//   subexpression, which is recomputed wherever it is used.
// * Function parameters are the data stack's contents. A summary's one
//   parameter is the object. A synthetic provider's getters receive the
//   object, then each value its init/update returned (the "attributes", in
//   order), then the method's own argument (the index or name). They are read
//   with `pick`, which counts from the bottom of the stack, so temporaries
//   above them never matter.
// * Values are typed (Object, Type, String, UInt, Int, Integer), because the
//   interpreter type-checks every operation. Mixing is resolved at compile
//   time: C++ integer literals take the type of the other operand, UInt
//   combined with Integer is converted with uint_to_integer<>, and anything
//   that cannot work is a static_assert rather than a runtime error in LLDB.
//
// Requires C++20.

#pragma once

#ifndef LLDBFMT_MODULE_BUILD
#include <array>
#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

#include "lldbfmt/lldbfmt.h"
#endif

namespace lldbfmt::script {

////////////////////////////////////////////////////////////////////////////////
// Types.
////////////////////////////////////////////////////////////////////////////////

struct object_tag { };  // SBValue.
struct type_tag { };    // SBType.
struct string_tag { };  // A String.
struct uint_tag { };    // Unsigned 64-bit: what get_value_as_unsigned, indexes and counts are.
struct int_tag { };     // Signed 64-bit: what get_value_as_signed is.
struct integer_tag { }; // Arbitrary precision: what get_byte_size is.
struct bool_tag { };    // The result of a comparison. Usable as a condition.
struct count_tag { };   // Internal: a child count or index (UInt, Int or Integer).
struct literal_int_tag { }; // Internal: a C++ integer literal that has not picked a type yet.
struct error_tag { };   // Internal: stands in for a type that could not be worked out, so one
                        // mistake gives one error rather than a cascade.

template<typename...>
inline constexpr bool dependent_false = false;

template<typename T>
concept node_like = requires(const T& node, byte_buffer& out) {
    typename T::tag;
    node.emit(out);
};

template<typename T>
concept integer_literal = std::integral<std::remove_cvref_t<T>> && !std::same_as<std::remove_cvref_t<T>, bool>;

template<typename T>
concept string_literal = (std::is_array_v<std::remove_cvref_t<T>> && std::same_as<std::remove_extent_t<std::remove_cvref_t<T>>, char>)
    || std::same_as<std::remove_cvref_t<T>, std::string_view>;

namespace impl {

template<typename T>
struct value_tag_of;
template<node_like T>
struct value_tag_of<T> { using type = typename T::tag; };
template<typename T> requires integer_literal<T>
struct value_tag_of<T> { using type = literal_int_tag; };
template<typename T> requires string_literal<T>
struct value_tag_of<T> { using type = string_tag; };

template<typename T>
using value_tag = typename value_tag_of<std::remove_cvref_t<T>>::type;

template<typename Tag>
inline constexpr bool is_numeric_tag = std::same_as<Tag, uint_tag> || std::same_as<Tag, int_tag> || std::same_as<Tag, integer_tag> || std::same_as<Tag, literal_int_tag> || std::same_as<Tag, error_tag>;

// The type two values have in common, if they can be given one. (No `type` when
// they cannot, so that this can be asked about without an error. common_tag below
// is what reports one.)
template<typename A, typename B>
struct unify_pair { };
template<typename A>
struct unify_pair<error_tag, A> {
    using type = error_tag;
};
template<typename A> requires (!std::same_as<A, error_tag>)
struct unify_pair<A, error_tag> {
    using type = error_tag;
};
template<typename A>
struct unify_pair<A, A> {
    using type = A;
};
template<typename A> requires is_numeric_tag<A>
struct unify_pair<literal_int_tag, A> {
    using type = A;
};
template<typename A> requires is_numeric_tag<A>
struct unify_pair<A, literal_int_tag> {
    using type = A;
};
template<>
struct unify_pair<literal_int_tag, literal_int_tag> {
    using type = literal_int_tag;
};
template<>
struct unify_pair<uint_tag, integer_tag> {
    using type = integer_tag;
};
template<>
struct unify_pair<integer_tag, uint_tag> {
    using type = integer_tag;
};

template<typename... Tags>
struct unify;
template<typename A>
struct unify<A> {
    using type = A;
};
template<typename A, typename B, typename... Rest> requires requires { typename unify_pair<A, B>::type; }
struct unify<A, B, Rest...> {
    using type = typename unify<typename unify_pair<A, B>::type, Rest...>::type;
};

template<typename Tag>
struct resolve {
    using type = Tag;
};
template<>
struct resolve<literal_int_tag> {
    using type = integer_tag;
};

template<typename... Values>
concept unifiable = requires { typename unify<value_tag<Values>...>::type; };

// The type the values have in common, or error_tag if there is none, which every
// later check lets through, so that one mistake is one error and not a screenful.
// (The mistake is reported by require_common_type, in a function body: a failed
// static_assert inside a class would make clang give up on the class and turn
// its members into other errors.)
template<bool Ok, typename... Values>
struct common_tag_of {
    using type = error_tag;
};
template<typename... Values>
struct common_tag_of<true, Values...> {
    using type = typename resolve<typename unify<value_tag<Values>...>::type>::type;
};

template<typename... Values>
using common_tag = typename common_tag_of<unifiable<Values...>, Values...>::type;

template<typename... Values>
constexpr void require_common_type()
{
    static_assert(unifiable<Values...>, "these values have no type in common (a UInt and an Integer can be combined, but an Int cannot be combined with either, and an Object, Type or String cannot be combined with anything else)");
}

} // namespace impl

////////////////////////////////////////////////////////////////////////////////
// Leaf nodes.
////////////////////////////////////////////////////////////////////////////////

// Converts a node or a C++ literal to a node of the expected type. UInt is
// promoted to Integer when an Integer is expected.
template<typename Expected, typename T>
constexpr auto convert(const T& value);

////////////////////////////////////////////////////////////////////////////////
// Calls into the interpreter.
////////////////////////////////////////////////////////////////////////////////

template<typename Result, selector Sel, typename... Operands>
constexpr auto make_call(const Operands&... operands);

template<opcode Op, typename Result, typename Operand>
struct unary;

////////////////////////////////////////////////////////////////////////////////
// The expression base class: SBValue / SBType / String methods.
////////////////////////////////////////////////////////////////////////////////

template<typename Derived, typename TagType>
struct expression {
    using tag = TagType;

    constexpr const Derived& self() const
    {
        return static_cast<const Derived&>(*this);
    }

    // SBValue.
    constexpr auto get_child_member_with_name(const auto& name) const requires std::same_as<TagType, object_tag>
    {
        return make_call<object_tag, selector::get_child_with_name>(self(), convert<string_tag>(name));
    }
    constexpr auto get_child_at_index(const auto& index) const requires std::same_as<TagType, object_tag>
    {
        return make_call<object_tag, selector::get_child_at_index>(self(), convert<uint_tag>(index));
    }
    constexpr auto get_index_of_child_with_name(const auto& name) const requires std::same_as<TagType, object_tag>
    {
        return make_call<uint_tag, selector::get_child_index>(self(), convert<string_tag>(name));
    }
    constexpr auto get_num_children() const requires std::same_as<TagType, object_tag>
    {
        return make_call<uint_tag, selector::get_num_children>(self());
    }
    constexpr auto get_parent() const requires std::same_as<TagType, object_tag>
    {
        return make_call<object_tag, selector::get_parent>(self());
    }
    constexpr auto get_synthetic_value() const requires std::same_as<TagType, object_tag>
    {
        return make_call<object_tag, selector::get_synthetic_value>(self());
    }
    constexpr auto get_non_synthetic_value() const requires std::same_as<TagType, object_tag>
    {
        return make_call<object_tag, selector::get_non_synthetic_value>(self());
    }
    constexpr auto clone(const auto& name) const requires std::same_as<TagType, object_tag>
    {
        return make_call<object_tag, selector::clone>(self(), convert<string_tag>(name));
    }
    constexpr auto get_type() const requires std::same_as<TagType, object_tag>
    {
        return make_call<type_tag, selector::get_type>(self());
    }
    constexpr auto get_value() const requires std::same_as<TagType, object_tag>
    {
        return make_call<string_tag, selector::get_value>(self());
    }
    constexpr auto get_summary() const requires std::same_as<TagType, object_tag>
    {
        return make_call<string_tag, selector::summary>(self());
    }
    constexpr auto get_value_as_unsigned() const requires std::same_as<TagType, object_tag>
    {
        return make_call<uint_tag, selector::get_value_as_unsigned>(self());
    }
    constexpr auto get_value_as_signed() const requires std::same_as<TagType, object_tag>
    {
        return make_call<int_tag, selector::get_value_as_signed>(self());
    }
    constexpr auto get_value_as_address() const requires std::same_as<TagType, object_tag>
    {
        return make_call<uint_tag, selector::get_value_as_address>(self());
    }
    constexpr auto cast(const auto& type) const requires std::same_as<TagType, object_tag>
    {
        return make_call<object_tag, selector::cast>(self(), convert<type_tag>(type));
    }
    constexpr auto create_child_at_offset(const auto& name, const auto& offset, const auto& type) const requires std::same_as<TagType, object_tag>
    {
        return make_call<object_tag, selector::create_child_at_offset>(self(), convert<string_tag>(name), convert<integer_tag>(offset), convert<type_tag>(type));
    }
    // `valobj is None` (and, as in Python, a missing child).
    constexpr auto is_null() const requires std::same_as<TagType, object_tag>
    {
        return unary<opcode::is_null, bool_tag, Derived> { { }, self() };
    }

    // `valobj.is_valid()`, and `if valobj:`.
    constexpr auto is_valid() const requires std::same_as<TagType, object_tag>;

    // SBType.
    constexpr auto get_pointee_type() const requires std::same_as<TagType, type_tag>
    {
        return make_call<type_tag, selector::get_pointee_type>(self());
    }
    constexpr auto get_byte_size() const requires std::same_as<TagType, type_tag>
    {
        return make_call<integer_tag, selector::get_byte_size>(self());
    }
    constexpr auto get_template_argument_type(const auto& index) const requires std::same_as<TagType, type_tag>
    {
        return make_call<type_tag, selector::get_template_argument_type>(self(), convert<uint_tag>(index));
    }

    // Strings: `len(s)`.
    constexpr auto length() const requires std::same_as<TagType, string_tag>
    {
        return make_call<uint_tag, selector::strlen>(self());
    }
};

// Reads a slot of the data stack: the object, an attribute, or an argument.
template<typename TagType, size_t Index>
struct local : expression<local<TagType, Index>, TagType> {
    constexpr void emit(byte_buffer& out) const
    {
        ::lldbfmt::detail::emit_unsigned_literal(out, Index);
        out.append(opcode::pick);
    }
};

struct uint_constant : expression<uint_constant, uint_tag> {
    uint64_t value;
    constexpr void emit(byte_buffer& out) const
    {
        ::lldbfmt::detail::emit_unsigned_literal(out, value);
    }
};

struct int_constant : expression<int_constant, int_tag> {
    int64_t value;
    constexpr void emit(byte_buffer& out) const
    {
        out.append(opcode::lit_int);
        out.append_sleb(value);
    }
};

struct integer_constant : expression<integer_constant, integer_tag> {
    int64_t value;
    constexpr void emit(byte_buffer& out) const
    {
        ::lldbfmt::detail::emit_integer_literal(out, value);
    }
};

struct string_constant : expression<string_constant, string_tag> {
    std::string_view value;
    constexpr void emit(byte_buffer& out) const
    {
        ::lldbfmt::detail::emit_string_literal(out, value);
    }
};

// `None`.
struct null_constant : expression<null_constant, object_tag> {
    constexpr void emit(byte_buffer& out) const
    {
        out.append(opcode::lit_null);
    }
};
inline constexpr null_constant null { };

template<opcode Op, typename Result, typename Operand>
struct unary : expression<unary<Op, Result, Operand>, Result> {
    Operand operand;
    constexpr void emit(byte_buffer& out) const
    {
        operand.emit(out);
        out.append(Op);
    }
};

// 1 for a valid object, 0 for a null one: 1 - is_null. No block is involved,
// so it can be used inside && and || without nesting conditionals.
template<typename Operand>
struct validity : expression<validity<Operand>, bool_tag> {
    Operand operand;
    constexpr void emit(byte_buffer& out) const
    {
        operand.emit(out);
        out.append(opcode::is_null);
        ::lldbfmt::detail::emit_unsigned_literal(out, 1);
        out.append(opcode::swap);
        out.append(opcode::minus);
    }
};

template<typename Derived, typename TagType>
constexpr auto expression<Derived, TagType>::is_valid() const requires std::same_as<TagType, object_tag>
{
    return validity<Derived> { { }, self() };
}

template<typename Result, selector Sel, typename... Operands>
struct call : expression<call<Result, Sel, Operands...>, Result> {
    static constexpr selector sel = Sel;
    std::tuple<Operands...> operands;

    constexpr void emit(byte_buffer& out) const
    {
        std::apply([&](const auto&... operand) { (operand.emit(out), ...); }, operands);
        ::lldbfmt::detail::emit_selector_literal(out, Sel);
        out.append(opcode::call);
    }
};

template<typename Result, selector Sel, typename... Operands>
constexpr auto make_call(const Operands&... operands)
{
    return call<Result, Sel, Operands...> { { }, std::tuple<Operands...>(operands...) };
}

// A UInt widened to an Integer.
template<typename Operand>
struct promote : expression<promote<Operand>, integer_tag> {
    Operand operand;
    constexpr void emit(byte_buffer& out) const
    {
        operand.emit(out);
        dsl::uint_to_integer<LLDBFMT_PROMOTION_BITS>().emit(out);
    }
};

template<typename Expected, typename T>
constexpr auto convert(const T& value)
{
    if constexpr (node_like<T>) {
        using actual = typename T::tag;
        if constexpr (std::same_as<actual, Expected> || std::same_as<Expected, error_tag> || std::same_as<actual, error_tag>)
            return value;
        else if constexpr (std::same_as<actual, uint_tag> && std::same_as<Expected, integer_tag>)
            return promote<T> { { }, value };
        else
            static_assert(dependent_false<T, Expected>, "expression has the wrong type for this position");
    } else if constexpr (integer_literal<T>) {
        if constexpr (std::same_as<Expected, uint_tag>) {
            if (value < 0)
                fail(error::negative_unsigned_literal);
            return uint_constant { { }, static_cast<uint64_t>(value) };
        } else if constexpr (std::same_as<Expected, int_tag>)
            return int_constant { { }, static_cast<int64_t>(value) };
        else if constexpr (std::same_as<Expected, integer_tag>)
            return integer_constant { { }, static_cast<int64_t>(value) };
        else
            static_assert(dependent_false<T, Expected>, "an integer literal is not valid here");
    } else if constexpr (string_literal<T>) {
        static_assert(std::same_as<Expected, string_tag>, "a string literal is not valid here");
        if constexpr (std::is_array_v<T>)
            return string_constant { { }, std::string_view(value, std::extent_v<T> - 1) };
        else
            return string_constant { { }, value };
    } else
        static_assert(dependent_false<T>, "not an expression or literal");
}

////////////////////////////////////////////////////////////////////////////////
// Operators.
////////////////////////////////////////////////////////////////////////////////

template<opcode Op, typename Result, typename Common, typename L, typename R>
struct binary_node : expression<binary_node<Op, Result, Common, L, R>, Result> {
    L left;
    R right;
    constexpr void emit(byte_buffer& out) const
    {
        left.emit(out);
        right.emit(out);
        out.append(Op);
        // Comparing two Ints yields an Int, which `if` rejects.
        if constexpr (std::same_as<Result, bool_tag> && std::same_as<Common, int_tag>)
            out.append(opcode::as_uint);
    }
};

template<typename L, typename R>
concept any_node = node_like<std::remove_cvref_t<L>> || node_like<std::remove_cvref_t<R>>;

namespace impl {

template<opcode Op, typename Result, typename L, typename R>
constexpr auto binary(const L& left, const R& right)
{
    require_common_type<L, R>();
    using Common = common_tag<L, R>;
    static_assert(is_numeric_tag<Common>, "arithmetic and comparison need numbers");
    auto l = convert<Common>(left);
    auto r = convert<Common>(right);
    using ResultTag = std::conditional_t<std::same_as<Result, void>, Common, Result>;
    return binary_node<Op, ResultTag, Common, decltype(l), decltype(r)> { { }, l, r };
}

template<opcode Op, typename L, typename R>
constexpr auto shift(const L& left, const R& right)
{
    // The count must be a UInt. The left side keeps its type.
    using Common = typename resolve<value_tag<L>>::type;
    auto l = convert<Common>(left);
    auto r = convert<uint_tag>(right);
    return binary_node<Op, Common, Common, decltype(l), decltype(r)> { { }, l, r };
}

} // namespace impl

#define LLDBFMT_SCRIPT_ARITHMETIC(OPERATOR, OPCODE) \
    template<typename L, typename R> requires any_node<L, R> \
    constexpr auto operator OPERATOR(const L& left, const R& right) \
    { \
        return impl::binary<opcode::OPCODE, void>(left, right); \
    }
LLDBFMT_SCRIPT_ARITHMETIC(+, plus)
LLDBFMT_SCRIPT_ARITHMETIC(-, minus)
LLDBFMT_SCRIPT_ARITHMETIC(*, mul)
LLDBFMT_SCRIPT_ARITHMETIC(/, div)
LLDBFMT_SCRIPT_ARITHMETIC(%, mod)
LLDBFMT_SCRIPT_ARITHMETIC(&, and_)
LLDBFMT_SCRIPT_ARITHMETIC(|, or_)
LLDBFMT_SCRIPT_ARITHMETIC(^, xor_)
#undef LLDBFMT_SCRIPT_ARITHMETIC

#define LLDBFMT_SCRIPT_COMPARISON(OPERATOR, OPCODE) \
    template<typename L, typename R> requires any_node<L, R> \
    constexpr auto operator OPERATOR(const L& left, const R& right) \
    { \
        return impl::binary<opcode::OPCODE, bool_tag>(left, right); \
    }
LLDBFMT_SCRIPT_COMPARISON(==, eq)
LLDBFMT_SCRIPT_COMPARISON(!=, neq)
LLDBFMT_SCRIPT_COMPARISON(<, lt)
LLDBFMT_SCRIPT_COMPARISON(>, gt)
LLDBFMT_SCRIPT_COMPARISON(<=, le)
LLDBFMT_SCRIPT_COMPARISON(>=, ge)
#undef LLDBFMT_SCRIPT_COMPARISON

template<typename L, typename R> requires node_like<L>
constexpr auto operator<<(const L& left, const R& right)
{
    return impl::shift<opcode::shl>(left, right);
}
template<typename L, typename R> requires node_like<L>
constexpr auto operator>>(const L& left, const R& right)
{
    return impl::shift<opcode::shr>(left, right);
}

template<node_like X>
constexpr auto operator~(const X& operand)
{
    static_assert(impl::is_numeric_tag<typename X::tag>, "~ needs a number");
    return unary<opcode::not_, typename X::tag, X> { { }, operand };
}

////////////////////////////////////////////////////////////////////////////////
// Control flow: `a if c else b`, and if/elif/else chains.
////////////////////////////////////////////////////////////////////////////////

namespace impl {

// `if` accepts Integer and UInt. An Int is reinterpreted (keeps zero-ness).
template<typename C>
constexpr auto condition(const C& value)
{
    using Tag = value_tag<C>;
    if constexpr (std::same_as<Tag, int_tag>)
        return unary<opcode::as_uint, uint_tag, C> { { }, value };
    else if constexpr (std::same_as<Tag, uint_tag> || std::same_as<Tag, integer_tag> || std::same_as<Tag, bool_tag>)
        return value;
    else if constexpr (integer_literal<C>)
        return convert<uint_tag>(value);
    else
        static_assert(dependent_false<C>, "a condition must be a number or a comparison");
}

} // namespace impl

template<typename Condition, typename Then, typename Else, typename Result>
struct conditional : expression<conditional<Condition, Then, Else, Result>, Result> {
    Condition condition;
    Then then;
    Else otherwise;

    constexpr void emit(byte_buffer& out) const
    {
        condition.emit(out);
        size_t start = out.size;
        then.emit(out);
        out.end_block(start);
        start = out.size;
        otherwise.emit(out);
        out.end_block(start);
        out.append(opcode::if_else);
    }
};

// if_(cond, a, b)                a if cond else b
// if_(c1, a1, c2, a2, ..., b)    if c1: a1 / elif c2: a2 / ... / else: b
template<typename C, typename T, typename E>
constexpr auto if_(const C& cond, const T& then, const E& otherwise)
{
    if constexpr (std::same_as<impl::value_tag<C>, object_tag>) {
        // `if valobj:` is true for a valid object.
        return if_(cond.is_null(), otherwise, then);
    } else if constexpr (std::same_as<impl::value_tag<C>, string_tag>) {
        // `if string:` is true for a non-empty string.
        return if_(cond.length(), then, otherwise);
    } else {
        impl::require_common_type<T, E>();
        using Common = impl::common_tag<T, E>;
        auto c = impl::condition(cond);
        auto t = convert<Common>(then);
        auto e = convert<Common>(otherwise);
        return conditional<decltype(c), decltype(t), decltype(e), Common> { { }, c, t, e };
    }
}

template<typename C, typename T, typename... Rest> requires (sizeof...(Rest) >= 2)
constexpr auto if_(const C& cond, const T& then, const Rest&... rest)
{
    return if_(cond, then, if_(rest...));
}

// Logical operators, evaluated left to right and short-circuiting. Their value
// is only good as a condition: the last operand evaluated. They are built from
// flat `dup { drop rhs } if` steps, not nested conditionals, so chains like
// `a && b && c` stay clear of the interpreter bug that drops the code after a
// conditional nested in another's block (see LLDBFMT_ALLOW_NESTED_CONDITIONALS).
template<bool IsAnd, typename L, typename R>
struct logical : expression<logical<IsAnd, L, R>, bool_tag> {
    L left;
    R right;

    constexpr void emit(byte_buffer& out) const
    {
        left.emit(out);
        out.append(opcode::dup);
        if constexpr (IsAnd) {
            // keep the left value if it is false, else replace it
            size_t start = out.size;
            out.append(opcode::drop);
            right.emit(out);
            out.end_block(start);
            out.append(opcode::if_);
        } else {
            size_t start = out.size;
            out.end_block(start); // keep the left value if it is true
            start = out.size;
            out.append(opcode::drop);
            right.emit(out);
            out.end_block(start);
            out.append(opcode::if_else);
        }
    }
};

namespace impl {

// A value usable as one side of && / ||: something `if` accepts.
template<typename X>
constexpr auto logical_operand(const X& value)
{
    using Tag = value_tag<X>;
    if constexpr (std::same_as<Tag, object_tag>)
        return value.is_valid();
    else if constexpr (std::same_as<Tag, string_tag>)
        return value.length();
    else
        return condition(value);
}

} // namespace impl

template<typename L, typename R> requires any_node<L, R>
constexpr auto operator&&(const L& left, const R& right)
{
    auto l = impl::logical_operand(left);
    auto r = impl::logical_operand(right);
    return logical<true, decltype(l), decltype(r)> { { }, l, r };
}

template<typename L, typename R> requires any_node<L, R>
constexpr auto operator||(const L& left, const R& right)
{
    auto l = impl::logical_operand(left);
    auto r = impl::logical_operand(right);
    return logical<false, decltype(l), decltype(r)> { { }, l, r };
}

template<node_like X>
constexpr auto operator!(const X& operand)
{
    return if_(operand, uint_constant { { }, 0 }, uint_constant { { }, 1 });
}

////////////////////////////////////////////////////////////////////////////////
// Lookup: the bytecode can compare numbers but not strings, so a chain of
// `if name == "m_size": ...` is written as a dictionary lookup instead.
// An unknown key is an error in LLDB (the formatter fails for that call).
////////////////////////////////////////////////////////////////////////////////

namespace impl {

// Builds the dictionary: `dict` then, for each entry, `dup key value dict_set`.
template<typename Entries>
constexpr void emit_dictionary(byte_buffer& out, const Entries& entries)
{
    out.append(opcode::dict);
    [&]<size_t... pair>(std::index_sequence<pair...>) {
        (([&] {
            out.append(opcode::dup);
            std::get<2 * pair>(entries).emit(out);
            std::get<2 * pair + 1>(entries).emit(out);
            out.append(opcode::dict_set);
        }()), ...);
    }(std::make_index_sequence<std::tuple_size_v<Entries> / 2>());
}

} // namespace impl

template<typename Key, typename Result, typename Entries>
struct lookup_node : expression<lookup_node<Key, Result, Entries>, Result> {
    Key key;
    Entries entries; // std::tuple of key, value, key, value, ...

    constexpr void emit(byte_buffer& out) const
    {
        impl::emit_dictionary(out, entries);
        key.emit(out);
        out.append(opcode::dict_get);
    }
};

template<typename K, typename... Rest> requires (sizeof...(Rest) >= 2 && sizeof...(Rest) % 2 == 0)
constexpr auto lookup(const K& key, const Rest&... rest)
{
    auto arguments = std::tie(rest...);
    return [&]<size_t... pair>(std::index_sequence<pair...>) {
        // Every value has the type they have in common.
        using Result = impl::common_tag<std::tuple_element_t<2 * pair + 1, std::tuple<Rest...>>...>;
        auto entries = std::tuple_cat(std::make_tuple(
            convert<string_tag>(std::get<2 * pair>(arguments)),
            convert<Result>(std::get<2 * pair + 1>(arguments)))...);
        auto k = convert<string_tag>(key);
        return lookup_node<decltype(k), Result, decltype(entries)> { { }, k, entries };
    }(std::make_index_sequence<sizeof...(Rest) / 2>());
}

// lookup(name, "k1", v1, "k2", v2, otherwise): `{"k1": v1, "k2": v2}.get(name, otherwise)`.
// `otherwise` is only evaluated when the key is absent.
template<typename Key, typename Result, typename Entries, typename Otherwise>
struct lookup_or_node : expression<lookup_or_node<Key, Result, Entries, Otherwise>, Result> {
    Key key;
    Entries entries;
    Otherwise otherwise;

    constexpr void emit(byte_buffer& out) const
    {
        impl::emit_dictionary(out, entries);
        key.emit(out);
        // dict key -> dict key dict key -> dict key found
        out.append(opcode::over);
        out.append(opcode::over);
        out.append(opcode::dict_has);
        size_t start = out.size;
        out.append(opcode::dict_get);
        out.end_block(start);
        start = out.size;
        out.append(opcode::drop);
        out.append(opcode::drop);
        otherwise.emit(out);
        out.end_block(start);
        out.append(opcode::if_else);
    }
};

template<typename K, typename... Rest> requires (sizeof...(Rest) >= 3 && sizeof...(Rest) % 2 == 1)
constexpr auto lookup(const K& key, const Rest&... rest)
{
    auto arguments = std::tie(rest...);
    constexpr size_t pairs = sizeof...(Rest) / 2;
    return [&]<size_t... pair>(std::index_sequence<pair...>) {
        using Result = impl::common_tag<std::tuple_element_t<2 * pair + 1, std::tuple<Rest...>>..., std::tuple_element_t<sizeof...(Rest) - 1, std::tuple<Rest...>>>;
        auto entries = std::tuple_cat(std::make_tuple(
            convert<string_tag>(std::get<2 * pair>(arguments)),
            convert<Result>(std::get<2 * pair + 1>(arguments)))...);
        auto k = convert<string_tag>(key);
        auto otherwise = convert<Result>(std::get<sizeof...(Rest) - 1>(arguments));
        return lookup_or_node<decltype(k), Result, decltype(entries), decltype(otherwise)> { { }, k, entries, otherwise };
    }(std::make_index_sequence<pairs>());
}

////////////////////////////////////////////////////////////////////////////////
// Format: Python's `"..." % (a, b)`. Understands %s, %d, %i, %u and %%.
// Braces need no escaping. Integers are formatted correctly even though the
// interpreter's @fmt mishandles UInt and Int: a get_value_as_*() read is
// formatted through get_value (same digits, as a String), and any other UInt
// is widened to an Integer.
////////////////////////////////////////////////////////////////////////////////

namespace impl {

template<typename T>
concept value_read = requires { T::sel; } && (T::sel == selector::get_value_as_unsigned || T::sel == selector::get_value_as_signed || T::sel == selector::get_value_as_address);

template<typename T>
constexpr void emit_format_argument(byte_buffer& out, const T& argument)
{
    using Tag = typename T::tag;
    if constexpr (value_read<T>) {
        std::get<0>(argument.operands).emit(out);
        ::lldbfmt::detail::emit_selector_literal(out, selector::get_value);
        out.append(opcode::call);
    } else if constexpr (std::same_as<Tag, uint_tag>)
        promote<T> { { }, argument }.emit(out);
    else if constexpr (std::same_as<Tag, int_tag> || std::same_as<Tag, bool_tag>)
        static_assert(dependent_false<T>, "cannot format a computed Int or comparison; format the get_value_as_signed() read itself, or an Integer");
    else
        argument.emit(out);
}

// Rewrites a Python-style format into llvm::formatv syntax. Returns the
// length and counts the specifiers.
constexpr size_t translate_format(std::string_view format, std::array<char, 512>& result, size_t& specifiers)
{
    size_t length = 0;
    auto put = [&](char c) {
        if (length == result.size())
            fail(error::buffer_overflow);
        result[length++] = c;
    };
    specifiers = 0;
    for (size_t i = 0; i < format.size(); ++i) {
        char c = format[i];
        if (c == '%') {
            if (++i == format.size())
                fail(error::unsupported_format_specifier);
            char kind = format[i];
            if (kind == '%')
                put('%');
            else if (kind == 's' || kind == 'd' || kind == 'i' || kind == 'u') {
                put('{');
                size_t index = specifiers++;
                if (index >= 10)
                    put(static_cast<char>('0' + (index / 10) % 10));
                put(static_cast<char>('0' + index % 10));
                put('}');
            } else
                fail(error::unsupported_format_specifier);
        } else if (c == '{') {
            put('{');
            put('{');
        } else
            put(c);
    }
    return length;
}

} // namespace impl

template<typename Arguments>
struct format_node : expression<format_node<Arguments>, string_tag> {
    std::string_view format;
    Arguments arguments;

    constexpr void emit(byte_buffer& out) const
    {
        std::array<char, 512> translated { };
        size_t specifiers = 0;
        size_t length = impl::translate_format(format, translated, specifiers);
        constexpr size_t count = std::tuple_size_v<Arguments>;
        if (specifiers != count)
            fail(error::format_specifier_count_mismatch);

        std::apply([&](const auto&... argument) { (impl::emit_format_argument(out, argument), ...); }, arguments);
        ::lldbfmt::detail::emit_string_literal(out, std::string_view(translated.data(), length));
        ::lldbfmt::detail::emit_selector_literal(out, selector::fmt);
        out.append(opcode::call);
        if constexpr (!LLDBFMT_FMT_CONSUMES_ARGUMENTS) {
            for (size_t i = 0; i < count; ++i) {
                out.append(opcode::swap);
                out.append(opcode::drop);
            }
        }
    }
};

template<size_t N, typename... Arguments>
constexpr auto format(const char (&text)[N], const Arguments&... arguments)
{
    auto converted = std::make_tuple([&] {
        if constexpr (node_like<Arguments>)
            return arguments;
        else if constexpr (integer_literal<Arguments>)
            return convert<integer_tag>(arguments);
        else
            return convert<string_tag>(arguments);
    }()...);
    return format_node<decltype(converted)> { { }, std::string_view(text, N - 1), converted };
}

////////////////////////////////////////////////////////////////////////////////
// Records.
////////////////////////////////////////////////////////////////////////////////

// A summary: a function from the object to a String.
//
//   summary("WebCore::IntRect", [](auto valobj) { return format(...); })
template<typename Function>
constexpr byte_buffer summary(std::string_view type_name, uint64_t flags, Function function)
{
    byte_buffer program;
    convert<string_tag>(function(local<object_tag, 0> { })).emit(program);
    return record(type_name, flags, method_program { signature::summary, program });
}

template<typename Function>
constexpr byte_buffer summary(std::string_view type_name, Function function)
{
    return summary(type_name, type_options::cascade, function);
}

// The attributes an init or update function adds to the stack, in order.
// Return one from init and update: `return state(buffer, size, data_size);`
template<typename... Attributes>
struct state_definition {
    std::tuple<Attributes...> attributes;
    bool reuse = false;
};

namespace impl {

template<typename Attribute>
constexpr auto to_attribute(const Attribute& attribute)
{
    if constexpr (node_like<Attribute>)
        return attribute;
    else if constexpr (integer_literal<Attribute>)
        return convert<integer_tag>(attribute);
    else
        return convert<string_tag>(attribute);
}

template<typename... Converted>
constexpr auto make_state(const std::tuple<Converted...>& converted, bool reuse)
{
    return state_definition<Converted...> { converted, reuse };
}

} // namespace impl

template<typename... Attributes>
constexpr auto state(const Attributes&... attributes)
{
    return impl::make_state(std::make_tuple(impl::to_attribute(attributes)...), false);
}

// Like `state`, for an update that returns True: the children LLDB already
// has are still valid, so it keeps them instead of refetching.
template<typename... Attributes>
constexpr auto state_reuse(const Attributes&... attributes)
{
    auto result = state(attributes...);
    result.reuse = true;
    return result;
}

enum class method_kind { init, update, num_children, child_index, child_at_index, value };

template<method_kind Kind, typename Function>
struct method_spec {
    static constexpr method_kind kind = Kind;
    Function function;
};

struct missing { };

// init(fn):            fn(valobj)                        -> state(...)
// update(fn):          fn(valobj, init attributes...)    -> state(...), runs on every refresh
// num_children(fn):    fn(valobj, attributes...)         -> a number
// child_index(fn):     fn(valobj, attributes..., name)   -> a number
// child_at_index(fn):  fn(valobj, attributes..., index)  -> Object
// value(fn):           fn(valobj, attributes...)         -> String
template<typename F>
constexpr auto init(F function)
{
    return method_spec<method_kind::init, F> { function };
}
template<typename F>
constexpr auto update(F function)
{
    return method_spec<method_kind::update, F> { function };
}
template<typename F>
constexpr auto num_children(F function)
{
    return method_spec<method_kind::num_children, F> { function };
}
template<typename F>
constexpr auto child_index(F function)
{
    return method_spec<method_kind::child_index, F> { function };
}
template<typename F>
constexpr auto child_at_index(F function)
{
    return method_spec<method_kind::child_at_index, F> { function };
}
template<typename F>
constexpr auto value(F function)
{
    return method_spec<method_kind::value, F> { function };
}

namespace impl {

template<method_kind Kind, typename... Specs>
constexpr auto find_method(const Specs&... specs)
{
    constexpr size_t found = ((Specs::kind == Kind ? 1 : 0) + ... + 0);
    static_assert(found <= 1, "a method is given twice");
    if constexpr (found == 0)
        return missing { };
    else {
        auto all = std::tie(specs...);
        constexpr std::array<method_kind, sizeof...(Specs)> kinds { Specs::kind... };
        constexpr size_t index = [&] {
            for (size_t i = 0; i < kinds.size(); ++i) {
                if (kinds[i] == Kind)
                    return i;
            }
            return size_t { 0 };
        }();
        return std::get<index>(all);
    }
}

template<size_t Offset, typename... Attributes, size_t... I>
constexpr auto slots_for(const state_definition<Attributes...>&, std::index_sequence<I...>)
{
    return std::tuple<local<typename Attributes::tag, Offset + I>...> { };
}

template<typename Spec, typename Locals>
constexpr auto run_state(const Spec& spec, const Locals& locals)
{
    if constexpr (std::same_as<Spec, missing>)
        return state_definition<> { };
    else {
        return std::apply(spec.function, locals);
    }
}

template<typename... Attributes>
constexpr void emit_state(byte_buffer& out, const state_definition<Attributes...>& state)
{
    std::apply([&](const auto&... attribute) { (attribute.emit(out), ...); }, state.attributes);
}

template<size_t Offset, typename Locals, typename... Attributes>
constexpr auto extend(const Locals& locals, const state_definition<Attributes...>& state)
{
    return std::tuple_cat(locals, slots_for<Offset>(state, std::make_index_sequence<sizeof...(Attributes)>()));
}

// The verifier's name for a tag.
template<typename Tag>
constexpr uint16_t kinds_of()
{
    if constexpr (std::same_as<Tag, object_tag>)
        return kinds::object;
    else if constexpr (std::same_as<Tag, type_tag>)
        return kinds::type;
    else if constexpr (std::same_as<Tag, string_tag>)
        return kinds::string;
    else if constexpr (std::same_as<Tag, uint_tag>)
        return kinds::unsigned_int;
    else if constexpr (std::same_as<Tag, int_tag>)
        return kinds::signed_int;
    else if constexpr (std::same_as<Tag, integer_tag>)
        return kinds::integer;
    else if constexpr (std::same_as<Tag, bool_tag>)
        return kinds::condition | kinds::signed_int;
    else
        return kinds::unknown;
}

// What the stack holds when a method starts: the locals, then its argument.
template<typename ArgumentTag, typename... Locals>
constexpr slot_types make_slot_types(const std::tuple<Locals...>&)
{
    slot_types types;
    types.open = false;
    ((types.kinds[types.count++] = kinds_of<typename Locals::tag>()), ...);
    if constexpr (!std::same_as<ArgumentTag, void>)
        types.kinds[types.count++] = kinds_of<ArgumentTag>();
    return types;
}

template<typename T>
constexpr bool is_wide_count()
{
    if constexpr (node_like<T>)
        return std::same_as<typename T::tag, integer_tag> || std::same_as<typename T::tag, int_tag>;
    else
        return false;
}

template<typename ArgumentTag, typename ResultTag, typename Spec, typename Locals>
constexpr byte_buffer getter(const Spec& spec, const Locals& locals)
{
    byte_buffer program;
    constexpr size_t argument = std::tuple_size_v<Locals>;
    auto all = [&] {
        if constexpr (std::same_as<ArgumentTag, void>)
            return locals;
        else
            return std::tuple_cat(locals, std::make_tuple(local<ArgumentTag, argument> { }));
    }();
    auto result = std::apply(spec.function, all);
    if constexpr (std::same_as<ResultTag, count_tag>) {
        // LLDB accepts a signed or arbitrary-precision count or index too.
        if constexpr (is_wide_count<decltype(result)>())
            result.emit(program);
        else
            convert<uint_tag>(result).emit(program);
    } else
        convert<ResultTag>(result).emit(program);
    return program;
}

template<typename ArgumentTag, typename ResultTag, typename Spec, typename Locals>
constexpr void add_getter(::lldbfmt::detail::program_set& programs, signature sig, const Spec& spec, const Locals& locals)
{
    if constexpr (!std::same_as<Spec, missing>)
        programs.set(sig, getter<ArgumentTag, ResultTag>(spec, locals), make_slot_types<ArgumentTag>(locals));
}

} // namespace impl

// A synthetic children provider, as the methods of a Python provider class.
template<typename... Specs>
constexpr byte_buffer synthetic(std::string_view type_name, uint64_t flags, const Specs&... specs)
{
    auto init = impl::find_method<method_kind::init>(specs...);
    auto update = impl::find_method<method_kind::update>(specs...);
    auto num_children = impl::find_method<method_kind::num_children>(specs...);
    auto child_index = impl::find_method<method_kind::child_index>(specs...);
    auto child_at_index = impl::find_method<method_kind::child_at_index>(specs...);
    auto value = impl::find_method<method_kind::value>(specs...);

    // The stack starts as [valobj], and init and update each append attributes.
    auto slots0 = std::make_tuple(local<object_tag, 0> { });
    auto init_state = impl::run_state(init, slots0);
    auto slots1 = impl::extend<std::tuple_size_v<decltype(slots0)>>(slots0, init_state);
    auto update_state = impl::run_state(update, slots1);
    auto slots2 = impl::extend<std::tuple_size_v<decltype(slots1)>>(slots1, update_state);

    ::lldbfmt::detail::program_set programs;

    if constexpr (!std::same_as<decltype(init), missing>) {
        byte_buffer program;
        impl::emit_state(program, init_state);
        programs.set(signature::init, program, impl::make_slot_types<void>(slots0));
    }
    if constexpr (!std::same_as<decltype(update), missing>) {
        byte_buffer program;
        impl::emit_state(program, update_state);
        // eRefetch (0) unless the update said the children are still valid (1).
        ::lldbfmt::detail::emit_unsigned_literal(program, update_state.reuse ? 1 : 0);
        programs.set(signature::update, program, impl::make_slot_types<void>(slots1));
    }
    impl::add_getter<void, count_tag>(programs, signature::get_num_children, num_children, slots2);
    impl::add_getter<string_tag, count_tag>(programs, signature::get_child_index, child_index, slots2);
    impl::add_getter<uint_tag, object_tag>(programs, signature::get_child_at_index, child_at_index, slots2);
    impl::add_getter<void, string_tag>(programs, signature::get_value, value, slots2);

    return ::lldbfmt::detail::serialize(type_name, flags, programs);
}

} // namespace lldbfmt::script

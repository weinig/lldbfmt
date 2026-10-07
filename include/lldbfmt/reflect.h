// Part of the lldbfmt project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// lldbfmt/reflect.h: formatters for your own types, written as
// annotations on them, using C++26 reflection.
//
//   namespace lldb = lldbfmt::reflect;
//
//   struct [[=lldb::summary("({x}, {y})")]] Point { int x; int y; };
//
//   struct [[=lldb::summary("{origin} to {size}")]] Rect { Point origin; Point size; };
//
//   struct Account {
//       char* username;
//       [[=lldb::hidden {}]] char* password;     // not shown by LLDB
//   };
//
//   struct [[=lldb::summary("{count} items")]] Buffer {
//       [[=lldb::array("count")]] int* data;     // shown as [0], [1], ...
//       unsigned count;
//   };
//
//   LLDBFMT_REFLECT(Point);
//   LLDBFMT_REFLECT(Rect);
//   LLDBFMT_REFLECT(Account);
//   LLDBFMT_REFLECT(Buffer);
//
// Reflection supplies what the other layers make you spell out: the member
// names (a `{placeholder}` that names no member is a compile error that lists
// the ones there are), their types (a scalar is shown by value, a member whose
// type has a `summary` of its own by that summary), and the type's name, which
// becomes a regex for a class template specialization.
//
// The generated formatters are ordinary Script DSL programs, so everything
// else applies to them too: they are verified, and they can be printed at
// compile time (see LLDBFMT_REFLECT_NAMED).
//
// Needs a compiler with P2996 reflection, P3394 annotations and P1306
// expansion statements: GCC 16 with -std=c++26 -freflection.

#pragma once

#if !defined(__cpp_impl_reflection)
#error "lldbfmt/reflect.h needs C++26 reflection (P2996) and annotations (P3394); with GCC 16 use -std=c++26 -freflection"
#endif

#ifndef LLDBFMT_MODULE_BUILD
#include <meta>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lldbfmt/script.h"
#endif

namespace lldbfmt::reflect {

// For `options(type_options::cascade | ...)`.
namespace type_options = ::lldbfmt::type_options;

////////////////////////////////////////////////////////////////////////////////
// Annotations.
////////////////////////////////////////////////////////////////////////////////

// A string that can be a value in an annotation or in a promoted constant
// array. Those must be structural types, which rules out std::string and
// std::string_view, so this is a pointer to storage that
// std::define_static_string promotes to static duration, and a length.
struct static_string {
    const char* data { nullptr };
    size_t length { 0 };

    constexpr static_string() = default;

    template<size_t N>
    consteval static_string(const char (&literal)[N])
        : data(std::define_static_string(std::string_view(literal, N - 1)))
        , length(N - 1)
    {
    }

    constexpr std::string_view view() const
    {
        return { data, length };
    }
};

// On a type: how LLDB summarizes it. `{member}` is that member's value, or its
// summary if its type has one (and a C string as text). `{outer.inner}` reaches
// into a member, and `{{` and `}}` are braces.
struct summary {
    static_string format;

    template<size_t N>
    consteval summary(const char (&text)[N])
        : format(text)
    {
    }
};

// On a member: leave it out of the children LLDB shows.
struct hidden { };

// On a member: show it under another name.
struct rename {
    static_string name;

    template<size_t N>
    consteval rename(const char (&text)[N])
        : name(text)
    {
    }
};

// On a pointer member: show `count` elements of it, as [0], [1], ..., before
// the other members. `count` names a sibling member.
struct array {
    static_string count;

    template<size_t N>
    consteval array(const char (&text)[N])
        : count(text)
    {
    }
};

// On a type: the type name (a regex if it starts with '^') LLDB should match,
// instead of the one derived from the type.
struct match {
    static_string name;

    template<size_t N>
    consteval match(const char (&text)[N])
        : name(text)
    {
    }
};

// On a type: the flags for its formatters (see type_options), such as
// `options(type_options::cascade | type_options::skip_pointers)`. The default is
// `type_options::cascade`.
struct options {
    uint64_t flags;

    consteval options(uint64_t value)
        : flags(value)
    {
    }
};

////////////////////////////////////////////////////////////////////////////////
// Reading the annotated type (all at compile time).
////////////////////////////////////////////////////////////////////////////////

namespace impl {

namespace script = ::lldbfmt::script;

template<typename Annotation>
consteval std::optional<Annotation> annotation_of(std::meta::info item)
{
    auto found = std::meta::annotations_of_with_type(item, ^^Annotation);
    if (found.empty())
        return std::nullopt;
    return std::meta::extract<Annotation>(found[0]);
}

template<typename Annotation>
consteval bool has_annotation(std::meta::info item)
{
    return !std::meta::annotations_of_with_type(item, ^^Annotation).empty();
}

consteval std::meta::info strip(std::meta::info type)
{
    return std::meta::dealias(std::meta::remove_cvref(std::meta::dealias(type)));
}

consteval std::string qualified_name(std::meta::info item)
{
    std::string name;
    std::meta::info parent = std::meta::parent_of(item);
    if (std::meta::is_namespace(parent) && !std::meta::has_identifier(parent)) {
        // The global (or an anonymous) namespace adds nothing.
    } else if (std::meta::is_namespace(parent) || std::meta::is_type(parent))
        name = qualified_name(parent) + "::";

    // A class template specialization has no identifier, only a display name.
    if (!std::meta::has_identifier(item))
        return name + std::string(std::meta::display_string_of(item));
    return name + std::string(std::meta::identifier_of(item));
}

// What LLDB should match: the qualified name, or for a specialization of a
// class template, a regex for any of them.
consteval std::string type_name_of(std::meta::info type)
{
    type = strip(type);
    if (auto matched = annotation_of<match>(type))
        return std::string(matched->name.view());
    if (std::meta::has_template_arguments(type))
        return "^" + qualified_name(std::meta::template_of(type)) + "<.+>$";
    return qualified_name(type);
}

consteval uint64_t options_of(std::meta::info type)
{
    if (auto opts = annotation_of<options>(strip(type)))
        return opts->flags;
    return ::lldbfmt::type_options::cascade;
}

consteval std::vector<std::meta::info> members_of(std::meta::info type)
{
    return std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked());
}

consteval std::vector<std::meta::info> bases_of(std::meta::info type)
{
    return std::meta::bases_of(type, std::meta::access_context::unchecked());
}

// Does this type show different children than its members (so LLDB needs a
// synthetic provider)?
consteval bool needs_children(std::meta::info type)
{
    for (std::meta::info member : members_of(type)) {
        if (has_annotation<hidden>(member) || has_annotation<rename>(member) || has_annotation<array>(member))
            return true;
    }
    return false;
}

// The name LLDB shows a child under: a member's (possibly renamed) name, or the
// type name of a base class.
consteval std::string shown_name(std::meta::info item)
{
    if (std::meta::is_base(item))
        return std::string(std::meta::display_string_of(std::meta::type_of(item)));
    if (auto rename_annotation = annotation_of<rename>(item))
        return std::string(rename_annotation->name.view());
    return std::string(std::meta::identifier_of(item));
}

// What a summary can name in `type`, as the children LLDB will have then.
// With a synthetic provider those are the base classes (by their type names)
// and the members that are not hidden, under the names they are shown with.
// Without one, LLDB finds members, and the base classes' members, by name.
consteval std::vector<std::meta::info> visible_entries(std::meta::info type, bool& synthetic)
{
    type = strip(type);
    synthetic = needs_children(type);
    std::vector<std::meta::info> result;
    if (synthetic) {
        for (std::meta::info base : bases_of(type))
            result.push_back(base);
    }
    for (std::meta::info member : members_of(type)) {
        if (synthetic && (has_annotation<hidden>(member) || has_annotation<array>(member)))
            continue;
        result.push_back(member);
    }
    if (!synthetic) {
        for (std::meta::info base : bases_of(type)) {
            bool ignored = false;
            for (std::meta::info entry : visible_entries(std::meta::type_of(base), ignored))
                result.push_back(entry);
        }
    }
    return result;
}

consteval std::string visible_names(std::meta::info type)
{
    bool synthetic = false;
    std::string list;
    for (std::meta::info entry : visible_entries(type, synthetic)) {
        if (!list.empty())
            list += ", ";
        list += synthetic ? shown_name(entry) : std::string(std::meta::identifier_of(entry));
    }
    return list.empty() ? "(none)" : list;
}

////////////////////////////////////////////////////////////////////////////////
// Plans: what to generate, as static data the templates below read.
////////////////////////////////////////////////////////////////////////////////

struct placeholder_plan {
    const static_string* path;
    size_t path_length;
    bool use_summary;
};

struct summary_plan {
    bool present { false };
    static_string format;
    std::span<const placeholder_plan> placeholders;
};

struct child_plan {
    static_string member; // As declared, which is how to find it.
    static_string shown;  // What LLDB shows.
    bool renamed;
    int base_index; // The position of a base class among the real children, or -1.
};

struct children_plan {
    bool present { false };
    bool has_array { false };
    static_string array_pointer;
    static_string array_count;
    std::span<const child_plan> visible;
};

consteval static_string keep(const std::string& text)
{
    static_string result;
    result.data = std::define_static_string(text);
    result.length = text.size();
    return result;
}

consteval std::vector<std::string> split(std::string_view text, char separator)
{
    std::vector<std::string> parts(1);
    for (char c : text) {
        if (c == separator)
            parts.emplace_back();
        else
            parts.back() += c;
    }
    return parts;
}

// Finds `name` among what LLDB will show for `type`, or says why it cannot.
consteval std::meta::info find_visible(std::meta::info type, std::string_view name, std::string& error, std::string_view context)
{
    bool synthetic = false;
    for (std::meta::info entry : visible_entries(type, synthetic)) {
        std::string shown = synthetic ? shown_name(entry) : std::string(std::meta::identifier_of(entry));
        if (shown == name)
            return entry;
    }

    // Not found: say why.
    for (std::meta::info member : members_of(strip(type))) {
        if (std::meta::identifier_of(member) != name)
            continue;
        if (has_annotation<hidden>(member))
            error = std::string(context) + ": '" + std::string(name) + "' is hidden, so LLDB's summary cannot read it";
        else if (has_annotation<array>(member))
            error = std::string(context) + ": '" + std::string(name) + "' is shown as its elements, not as a member";
        else
            error = std::string(context) + ": '" + std::string(name) + "' is shown as '" + shown_name(member) + "'; use that name";
        return std::meta::info { };
    }
    error = std::string(context) + ": no member '" + std::string(name) + "' (members: " + visible_names(type) + ")";
    return std::meta::info { };
}

consteval bool is_c_string(std::meta::info type)
{
    return (std::meta::is_pointer_type(type) && std::meta::remove_cv(std::meta::remove_pointer(type)) == ^^char)
        || (std::meta::is_array_type(type) && std::meta::remove_cv(std::meta::remove_extent(type)) == ^^char);
}

consteval summary_plan summary_plan_of(std::meta::info type, std::string& error)
{
    type = strip(type);
    auto annotation = annotation_of<summary>(type);
    if (!annotation)
        return { };

    std::string context = "summary of " + qualified_name(type);
    std::string_view text = annotation->format.view();
    std::string format;
    std::vector<placeholder_plan> placeholders;

    for (size_t i = 0; i < text.size() && error.empty(); ++i) {
        char c = text[i];
        if (c == '{' && i + 1 < text.size() && text[i + 1] == '{') {
            format += '{';
            ++i;
        } else if (c == '}' && i + 1 < text.size() && text[i + 1] == '}') {
            format += '}';
            ++i;
        } else if (c == '{') {
            size_t close = text.find('}', i);
            if (close == std::string_view::npos) {
                error = context + ": '{' without a '}'";
                break;
            }
            std::string_view name = text.substr(i + 1, close - i - 1);
            std::vector<std::string> parts = split(name, '.');

            // Walk the path, checking each name exists where LLDB will look.
            std::meta::info current = type;
            std::meta::info entry;
            for (const std::string& part : parts) {
                entry = find_visible(current, part, error, context);
                if (!error.empty())
                    break;
                current = strip(std::meta::type_of(entry));
                while (std::meta::is_pointer_type(current))
                    current = strip(std::meta::remove_pointer(current)); // LLDB looks through pointers.
            }
            if (error.empty()) {
                std::meta::info last = strip(std::meta::type_of(entry));
                bool use_summary = false;
                if (is_c_string(last))
                    use_summary = true; // A C string shows as text through the summary.
                else if (std::meta::is_class_type(last)) {
                    if (!has_annotation<summary>(last)) {
                        std::string first = visible_names(last);
                        first = first.substr(0, first.find(','));
                        error = context + ": '{" + std::string(name) + "}' is a " + qualified_name(last)
                            + ", which has no summary; name one of its members, such as {" + std::string(name) + "." + first + "}";
                        break;
                    }
                    use_summary = true;
                }
                std::vector<static_string> path;
                for (const std::string& part : parts)
                    path.push_back(keep(part));
                placeholders.push_back({ std::define_static_array(path).data(), path.size(), use_summary });
                format += "%s";
            }
            i = close;
        } else if (c == '%')
            format += "%%";
        else
            format += c;
    }
    if (!error.empty())
        return { };
    return { true, keep(format), std::define_static_array(placeholders) };
}

consteval children_plan children_plan_of(std::meta::info type, std::string& error)
{
    type = strip(type);
    if (!needs_children(type))
        return { };

    children_plan plan;
    plan.present = true;
    std::vector<child_plan> visible;
    std::string context = "children of " + qualified_name(type);

    // Base classes come first, as LLDB lists them. The real child at the same
    // position is passed through.
    int base_index = 0;
    for (std::meta::info base : bases_of(type)) {
        static_string name = keep(shown_name(base));
        visible.push_back({ name, name, false, base_index++ });
    }

    for (std::meta::info member : members_of(type)) {
        std::string name(std::meta::identifier_of(member));
        if (auto array_annotation = annotation_of<array>(member)) {
            if (plan.has_array) {
                error = context + ": only one member can be an array";
                return { };
            }
            if (!std::meta::is_pointer_type(strip(std::meta::type_of(member)))) {
                error = context + ": array(\"" + std::string(array_annotation->count.view()) + "\") belongs on a pointer, but '" + name + "' is not one";
                return { };
            }
            bool found = false;
            for (std::meta::info other : members_of(type)) {
                if (std::meta::identifier_of(other) == array_annotation->count.view())
                    found = true;
            }
            if (!found) {
                error = context + ": array(\"" + std::string(array_annotation->count.view()) + "\") names no member (members: " + visible_names(type) + ")";
                return { };
            }
            plan.has_array = true;
            plan.array_pointer = keep(name);
            plan.array_count = keep(std::string(array_annotation->count.view()));
            continue;
        }
        if (has_annotation<hidden>(member))
            continue;
        bool renamed = has_annotation<rename>(member);
        visible.push_back({ keep(name), keep(shown_name(member)), renamed, -1 });
    }
    plan.visible = std::define_static_array(visible);
    return plan;
}

// Everything known about one annotated type.
template<typename T>
struct type_formatter {
    static constexpr std::string_view type_name = []() consteval {
        return keep(type_name_of(^^T)).view();
    }();

    static constexpr uint64_t flags = options_of(^^T);

    static constexpr auto plans = []() consteval {
        struct plans_result {
            summary_plan summary;
            children_plan children;
            static_string error;
        } result;
        std::string error;
        result.summary = summary_plan_of(^^T, error);
        if (error.empty())
            result.children = children_plan_of(^^T, error);
        result.error = keep(error);
        return result;
    }();

    static constexpr const summary_plan& summary = plans.summary;
    static constexpr const children_plan& children = plans.children;
    static constexpr std::string_view error = plans.error.view();
};

////////////////////////////////////////////////////////////////////////////////
// Building the formatter programs from the plans, with the Script DSL.
////////////////////////////////////////////////////////////////////////////////

// {a.b.c}: reads the member at the end of the path, as a string.
template<typename T, size_t I, size_t K = 0, typename Node>
constexpr auto placeholder_node(Node node)
{
    constexpr const placeholder_plan& placeholder = type_formatter<T>::summary.placeholders[I];
    if constexpr (K == placeholder.path_length) {
        if constexpr (placeholder.use_summary)
            return node.get_summary();
        else
            return node.get_value();
    } else
        return placeholder_node<T, I, K + 1>(node.get_child_member_with_name(placeholder.path[K].view()));
}

template<typename T>
constexpr byte_buffer summary_record()
{
    return script::summary(type_formatter<T>::type_name, type_formatter<T>::flags, [](auto valobj) {
        return [&]<size_t... I>(std::index_sequence<I...>) {
            auto arguments = std::make_tuple(placeholder_node<T, I>(valobj)...);
            return script::format_node<decltype(arguments)> { { }, type_formatter<T>::summary.format.view(), arguments };
        }(std::make_index_sequence<type_formatter<T>::summary.placeholders.size()>());
    });
}

// The number of array elements, read where LLDB will find it.
template<typename T>
constexpr auto array_count(auto valobj)
{
    return valobj.get_child_member_with_name(type_formatter<T>::children.array_count.view()).get_value_as_unsigned();
}

template<typename T, size_t J>
constexpr auto child_node(auto valobj)
{
    constexpr const child_plan& child = type_formatter<T>::children.visible[J];
    if constexpr (child.base_index >= 0)
        return valobj.get_child_at_index(child.base_index);
    else {
        auto member = valobj.get_child_member_with_name(child.member.view());
        if constexpr (child.renamed)
            return member.clone(child.shown.view());
        else
            return member;
    }
}

// Child J's position among all children: after any array elements.
template<typename T, size_t J>
constexpr auto child_position(auto valobj)
{
    if constexpr (type_formatter<T>::children.has_array)
        return array_count<T>(valobj) + J;
    else
        return J;
}

template<typename T, size_t J = 0>
constexpr auto member_chain(auto valobj, auto index)
{
    if constexpr (J == type_formatter<T>::children.visible.size())
        return script::null;
    else
        return script::if_(index == child_position<T, J>(valobj), child_node<T, J>(valobj), member_chain<T, J + 1>(valobj, index));
}

template<typename T>
constexpr byte_buffer synthetic_record()
{
    // (The lambdas read the plan through type_formatter<T>, not through locals,
    // so they need no captures.)
    auto num_children = script::num_children([](auto valobj) {
        if constexpr (type_formatter<T>::children.has_array)
            return array_count<T>(valobj) + type_formatter<T>::children.visible.size();
        else
            return type_formatter<T>::children.visible.size();
    });

    auto child_at_index = script::child_at_index([](auto valobj, auto index) {
        if constexpr (type_formatter<T>::children.has_array) {
            auto pointer = valobj.get_child_member_with_name(type_formatter<T>::children.array_pointer.view());
            auto pointee = pointer.get_type().get_pointee_type();
            auto element = pointer.create_child_at_offset(script::format("[%s]", index), index * pointee.get_byte_size(), pointee);
            return script::if_(index < array_count<T>(valobj), element, member_chain<T>(valobj, index));
        } else
            return member_chain<T>(valobj, index);
    });

    if constexpr (type_formatter<T>::children.visible.size() == 0)
        return script::synthetic(type_formatter<T>::type_name, type_formatter<T>::flags, num_children, child_at_index);
    else {
        // Looking a child up by name: the members, but not the array's elements.
        auto child_index = script::child_index([](auto valobj, auto name) {
            return [&]<size_t... J>(std::index_sequence<J...>) {
                return std::apply([&](const auto&... pairs) { return script::lookup(name, pairs...); },
                    std::tuple_cat(std::make_tuple(type_formatter<T>::children.visible[J].shown.view(), child_position<T, J>(valobj))...));
            }(std::make_index_sequence<type_formatter<T>::children.visible.size()>());
        });
        return script::synthetic(type_formatter<T>::type_name, type_formatter<T>::flags, num_children, child_index, child_at_index);
    }
}

// The records for a type: [summary][synthetic], either of which may be empty.
template<typename T>
constexpr byte_buffer summary_record_or_nothing()
{
    if constexpr (type_formatter<T>::summary.present)
        return summary_record<T>();
    else
        return byte_buffer { };
}

template<typename T>
constexpr byte_buffer synthetic_record_or_nothing()
{
    if constexpr (type_formatter<T>::children.present)
        return synthetic_record<T>();
    else
        return byte_buffer { };
}

template<const byte_buffer& SummaryBytes, const byte_buffer& SyntheticBytes>
consteval auto concatenated()
{
    std::array<uint8_t, SummaryBytes.size + SyntheticBytes.size> result { };
    for (size_t i = 0; i < SummaryBytes.size; ++i)
        result[i] = SummaryBytes.data[i];
    for (size_t i = 0; i < SyntheticBytes.size; ++i)
        result[SummaryBytes.size + i] = SyntheticBytes.data[i];
    return result;
}

} // namespace impl

} // namespace lldbfmt::reflect

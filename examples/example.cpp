// Build:  clang++ -std=c++20 -g -Iinclude examples/example.cpp -o example
// Try:    lldb -b -o 'b stop' -o run -o up -o 'frame variable' ./example

#include "lldbfmt/lldbfmt.h"

using namespace lldbfmt;
using namespace lldbfmt::dsl;

struct Point {
    int x;
    int y;
};

// Summary written in the DSL: "(x, y)".
//
//   stack: obj
//   dup "x" @get_child_with_name call @get_value call   -> obj xString
//   swap "y" @get_child_with_name call @get_value call  -> xString yString
//   "({0}, {1})" @fmt call                              -> xString yString result
LLDBFMT_FORMATTER(PointSummary, "Point", 0,
    summary(
        dup, "x", selector::get_child_with_name, call, selector::get_value, call,
        swap, "y", selector::get_child_with_name, call, selector::get_value, call,
        "({0}, {1})", selector::fmt, call,
        return_));

struct Box {
    Point origin;
    Point size;
};

// The same thing, as assembly text, matched with a regex.
LLDBFMT_ASSEMBLY(BoxSummary, "^Box$", 0,
    R"(@summary:
        dup "origin" @get_child_with_name call @summary call
        swap "size" @get_child_with_name call @summary call
        "origin={0} size={1}" @fmt call
        return)");

// A template with a regex, and a conditional.
template<typename T> struct Optional {
    bool hasValue;
    T value;
};

LLDBFMT_FORMATTER(OptionalSummary, "^Optional<.+>$", 0,
    summary(
        dup, "hasValue", selector::get_child_with_name, call, selector::get_value_as_unsigned, call,
        if_then_else(
            block("value", selector::get_child_with_name, call, selector::get_value, call, return_),
            block(drop, "None", return_))));

// A synthetic children provider: shows Pair's members in reverse order.
struct pair {
    int first;
    int second;
};

LLDBFMT_FORMATTER(PairChildren, "Pair", 0,
    get_num_children(drop, 2u),
    get_child_at_index(
        // stack: obj index  ->  obj (1 - index)  ->  child
        1u, swap, minus, selector::get_child_at_index, call,
        return_));

void stop() { }

int main()
{
    Point p { 3, 4 };
    Box b { { 1, 2 }, { 30, 40 } };
    Optional<int> some { true, 7 };
    Optional<int> none { false, 0 };
    pair pair { 10, 20 };
    stop();
    return p.x + b.size.x + some.value + none.value + pair.first;
}

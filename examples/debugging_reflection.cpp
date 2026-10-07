// The compile-time errors lldbfmt/reflect.h gives for a mistake in an
// annotation, and what it generates for a correct one. Needs GCC 16:
//
//   g++-16 -std=c++26 -freflection -fsyntax-only -Iinclude -DDEMO=1 examples/debugging_reflection.cpp
//
// or all of them: make debugging-demo

#include "lldbfmt/reflect.h"

namespace lldb = lldbfmt::reflect;

#ifndef DEMO
#define DEMO 0
#endif

#if DEMO == 0
// DEMO 0: nothing is wrong. This builds.
struct [[=lldb::summary("({x}, {y})")]] Point {
    int x;
    int y;
};
LLDBFMT_REFLECT(Point);

#elif DEMO == 1
// DEMO 1: a typo in a placeholder. Reflection knows the members, so the error
// lists them.
struct [[=lldb::summary("({x}, {z})")]] Point {
    int x;
    int y;
};
LLDBFMT_REFLECT(Point);

#elif DEMO == 2
// DEMO 2: a summary that names a hidden member. LLDB's summary would be looking at
// the synthetic children, which do not have it.
struct [[=lldb::summary("{user} {password}")]] Account {
    const char* user;
    [[=lldb::hidden {}]] const char* password;
};
LLDBFMT_REFLECT(Account);

#elif DEMO == 3
// DEMO 3: a renamed member, still called by its old name. The error says which
// name to use.
struct [[=lldb::summary("{name}")]] Account {
    [[=lldb::rename("user")]] const char* name;
};
LLDBFMT_REFLECT(Account);

#elif DEMO == 4
// DEMO 4: a member of class type that has no summary has nothing to show, and the error
// suggests one of its members.
struct Point {
    int x;
    int y;
};
struct [[=lldb::summary("at {where}")]] Marker {
    Point where;
};
LLDBFMT_REFLECT(Marker);

#elif DEMO == 5
// DEMO 5: an array on something that is not a pointer.
struct Buffer {
    [[=lldb::array("count")]] int data;
    unsigned count;
};
LLDBFMT_REFLECT(Buffer);

#elif DEMO == 6
// DEMO 6: what was generated. The summary of a Point as bytecode: each {member}
// became a read of that child as a string, and the format string was translated
// (`%s` for each). LLDBFMT_REFLECT_NAMED gives the records names so they can
// be printed. It is also how to look at the children a hidden or array member
// made.
struct [[=lldb::summary("({x}, {y})")]] Point {
    int x;
    int y;
};
LLDBFMT_REFLECT_NAMED(Point, generated);
LLDBFMT_PRINT_RECORD(generated_summary_record);

#elif DEMO == 7
// DEMO 7: the children generated for an array and a hidden member: a synthetic
// provider whose child_at_index reads the elements (the code that promotes the
// UInt index to an Integer is the long run of `dup N >> 1 & { ... } if`).
struct [[=lldb::summary("{count} items")]] Buffer {
    [[=lldb::array("count")]] int* data;
    unsigned count;
    [[=lldb::hidden {}]] unsigned capacity;
};
LLDBFMT_REFLECT_NAMED(Buffer, generated);
LLDBFMT_PRINT_RECORD(generated_synthetic_record);

#endif

int main()
{
}

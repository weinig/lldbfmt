// Formatters for your own types, as annotations on them (needs C++26
// reflection, see lldbfmt/reflect.h):
//
//   g++-16 -std=c++26 -freflection -g -Iinclude examples/reflection.cpp -o build/reflection
//   lldb -b -o 'b stop' -o run -o up -o 'frame variable' build/reflection

#include "lldbfmt/reflect.h"

namespace lldb = lldbfmt::reflect;

namespace Geometry {

struct [[=lldb::summary("({x}, {y})")]] Point {
    int x;
    int y;
};

// A member whose type has a `summary` is shown by that summary. {origin.x}
// would reach into it instead.
struct [[=lldb::summary("{origin} + {size}")]] Rect {
    Point origin;
    Point size;
};

} // namespace Geometry

// The password is not shown, and `name` is shown as `user`.
struct [[=lldb::summary("{user}")]] Account {
    [[=lldb::rename("user")]] const char* name;
    [[=lldb::hidden {}]] const char* password;
    int logins;
};

// `data` is shown as `count` elements, then the other members.
template<typename T>
struct [[=lldb::summary("{count} items")]] Buffer {
    [[=lldb::array("count")]] T* data;
    unsigned count;
};

enum class Mode { Fast, Slow };

struct [[=lldb::summary("{mode} {rect.origin.x}")]] Job {
    Mode mode;
    Geometry::Rect rect;
};

// A base class is a child, shown as its type name, and a summary can name it.
struct [[=lldb::summary("{label} at {Geometry::Point}")]] Marker : Geometry::Point {
    const char* label;
    [[=lldb::hidden {}]] int internalId;
};

using IntBuffer = Buffer<int>;

LLDBFMT_REFLECT(Geometry::Point);
LLDBFMT_REFLECT(Geometry::Rect);
LLDBFMT_REFLECT(Account);
LLDBFMT_REFLECT(IntBuffer);
LLDBFMT_REFLECT(Job);
LLDBFMT_REFLECT(Marker);

void stop() { }

int main()
{
    Geometry::Point point { 3, 4 };
    Geometry::Rect rect { { 1, 2 }, { 30, 40 } };
    Account account { "ada", "hunter2", 7 };
    int numbers[] = { 10, 20, 30 };
    IntBuffer buffer { numbers, 3 };
    Buffer<int> plain { numbers, 2 };
    Job job { Mode::Slow, rect };
    Marker marker { { 5, 6 }, "pin", 99 };
    stop();
    return point.x + rect.size.x + account.logins + buffer.data[0] + plain.count + static_cast<int>(job.mode) + marker.internalId;
}

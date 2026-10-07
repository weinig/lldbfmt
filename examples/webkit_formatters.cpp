// Reimplementations of three of WebKit's LLDB formatters (Tools/lldb/lldb_webkit.py)
// as embedded bytecode, written with the Python-shaped DSL (lldbfmt/script.h).
// The original Python is quoted above each one. See webkit_formatters_lowlevel.cpp
// for the same formatters in the low-level, stack-oriented DSL.
//
// The types at the bottom only mirror the member layout of the WebKit classes,
// so this builds without WebKit.
//
// Build:  clang++ -std=c++20 -g -Iinclude examples/webkit_formatters.cpp -o build/webkit_formatters
// Try:    lldb -b -o 'b stop' -o run -o up -o 'frame variable' build/webkit_formatters
//
// The synthetic provider needs a recent LLDB (@get_pointee_type, @get_byte_size,
// @create_child_at_offset, dictionaries and Integer literals were added after
// Apple's LLDB 2103). The summaries work with older ones too.

#include "lldbfmt/script.h"

#include <cstddef>

using namespace lldbfmt::script;

////////////////////////////////////////////////////////////////////////////////
// WebCore::IntRect
////////////////////////////////////////////////////////////////////////////////

// def WebCoreIntRect_SummaryProvider(valobj, dict):
//     provider = WebCoreIntRectProvider(valobj, dict)
//     return "{ x = %s, y = %s, width = %s, height = %s }" % (provider.get_x(), provider.get_y(), provider.get_width(), provider.get_height())
// ...
// class WebCoreIntPointProvider:
//     "Print a WebCore::IntPoint"
//     def __init__(self, valobj, dict):
//         self.valobj = valobj
//
//     def get_x(self):
//         return self.valobj.GetChildMemberWithName('m_x').GetValueAsSigned()
//
//     def get_y(self):
//         return self.valobj.GetChildMemberWithName('m_y').GetValueAsSigned()
//
// ...
// class WebCoreIntSizeProvider:
//     "Print a WebCore::IntSize"
//     def __init__(self, valobj, dict):
//         self.valobj = valobj
//
//     def get_width(self):
//         return self.valobj.GetChildMemberWithName('m_width').GetValueAsSigned()
//
//     def get_height(self):
//         return self.valobj.GetChildMemberWithName('m_height').GetValueAsSigned()
//
// ...
// class WebCoreIntRectProvider:
//     "Print a WebCore::IntRect"
//     def __init__(self, valobj, dict):
//         self.valobj = valobj
//
//     def get_x(self):
//         return WebCoreIntPointProvider(self.valobj.GetChildMemberWithName('m_location'), dict).get_x()
//
//     def get_y(self):
//         return WebCoreIntPointProvider(self.valobj.GetChildMemberWithName('m_location'), dict).get_y()
//
//     def get_width(self):
//         return WebCoreIntSizeProvider(self.valobj.GetChildMemberWithName('m_size'), dict).get_width()
//
//     def get_height(self):
//         return WebCoreIntSizeProvider(self.valobj.GetChildMemberWithName('m_size'), dict).get_height()
//
//
// The Python classes become C++ templates over the expression they wrap.
// `valobj.get_child_member_with_name(...)` builds an expression rather than
// calling LLDB, and the whole thing becomes bytecode at compile time.
template<typename value>
struct WebCoreIntPointProvider {
    value valobj;

    constexpr auto get_x() const { return valobj.get_child_member_with_name("m_x").get_value_as_signed(); }
    constexpr auto get_y() const { return valobj.get_child_member_with_name("m_y").get_value_as_signed(); }
};

template<typename value>
struct WebCoreIntSizeProvider {
    value valobj;

    constexpr auto get_width() const { return valobj.get_child_member_with_name("m_width").get_value_as_signed(); }
    constexpr auto get_height() const { return valobj.get_child_member_with_name("m_height").get_value_as_signed(); }
};

template<typename value>
struct WebCoreIntRectProvider {
    value valobj;

    constexpr auto get_x() const { return WebCoreIntPointProvider { valobj.get_child_member_with_name("m_location") }.get_x(); }
    constexpr auto get_y() const { return WebCoreIntPointProvider { valobj.get_child_member_with_name("m_location") }.get_y(); }
    constexpr auto get_width() const { return WebCoreIntSizeProvider { valobj.get_child_member_with_name("m_size") }.get_width(); }
    constexpr auto get_height() const { return WebCoreIntSizeProvider { valobj.get_child_member_with_name("m_size") }.get_height(); }
};

LLDBFMT_SCRIPT_SUMMARY(IntRectSummary, "WebCore::IntRect", [](auto valobj) {
    auto provider = WebCoreIntRectProvider { valobj };
    return format("{ x = %s, y = %s, width = %s, height = %s }", provider.get_x(), provider.get_y(), provider.get_width(), provider.get_height());
});

////////////////////////////////////////////////////////////////////////////////
// WTF::Vector<...> summary
////////////////////////////////////////////////////////////////////////////////

// def WTFVector_SummaryProvider(valobj, dict):
//     provider = WTFVectorProvider(valobj, dict)
//     return "{ size = %d, capacity = %d }" % (provider.size, provider.capacity)
//
// ...
// class WTFVectorProvider:
//     def __init__(self, valobj, internal_dict):
//         self.valobj = valobj
//         self.update()
//
// ...
//     def update(self):
//         self.buffer = self.valobj.GetChildMemberWithName('m_buffer')
//         self.size = self.valobj.GetChildMemberWithName('m_size').GetValueAsUnsigned(0)
//         self.capacity = self.valobj.GetChildMemberWithName('m_capacity').GetValueAsUnsigned(0)
//         self.data_type = self.buffer.GetType().GetPointeeType()
//         self.data_size = self.data_type.GetByteSize()
//
//
// The Python registers this with `-x "^WTF::Vector<.+>$"`. A type name that
// starts with '^' is a regex here too. size and capacity are read directly
// instead of through an update() step.
LLDBFMT_SCRIPT_SUMMARY(VectorSummary, "^WTF::Vector<.+>$", [](auto valobj) {
    auto size = valobj.get_child_member_with_name("m_size").get_value_as_unsigned();
    auto capacity = valobj.get_child_member_with_name("m_capacity").get_value_as_unsigned();
    return format("{ size = %d, capacity = %d }", size, capacity);
});

////////////////////////////////////////////////////////////////////////////////
// WTF::Vector<...> synthetic children: [0] ... [size-1], then m_size,
// m_capacity and m_buffer.
////////////////////////////////////////////////////////////////////////////////

// class WTFVectorProvider:
//     def __init__(self, valobj, internal_dict):
//         self.valobj = valobj
//         self.update()
//
//     def num_children(self):
//         return self.size + 3
//
//     def get_child_index(self, name):
//         if name == "m_size":
//             return self.size
//         elif name == "m_capacity":
//             return self.size + 1
//         elif name == "m_buffer":
//             return self.size + 2
//         else:
//             return int(name.lstrip('[').rstrip(']'))
//
//     def get_child_at_index(self, index):
//         if index == self.size:
//             return self.valobj.GetChildMemberWithName("m_size")
//         elif index == self.size + 1:
//             return self.valobj.GetChildMemberWithName("m_capacity")
//         elif index == self.size + 2:
//             return self.buffer
//         elif index < self.size:
//             offset = index * self.data_size
//             child = self.buffer.CreateChildAtOffset('[' + str(index) + ']', offset, self.data_type)
//             return child
//         else:
//             return None
//
//     def update(self):
//         self.buffer = self.valobj.GetChildMemberWithName('m_buffer')
//         self.size = self.valobj.GetChildMemberWithName('m_size').GetValueAsUnsigned(0)
//         self.capacity = self.valobj.GetChildMemberWithName('m_capacity').GetValueAsUnsigned(0)
//         self.data_type = self.buffer.GetType().GetPointeeType()
//         self.data_size = self.data_type.GetByteSize()
//
//     def has_children(self):
//         return True
//
//
// Notes on the translation:
//
// * The Python's attributes (`self.buffer`, `self.size`, `self.data_size`) are
//   what update() returns in state(...). Every other method receives them as
//   parameters, after valobj, and before its own argument (the index or the
//   name), so the C++ parameter names stand in for `self.`.
//
// * `offset = index * self.data_size` multiplies a UInt by an Integer, and the
//   DSL widens the index automatically.
//
// * `has_children` has no bytecode equivalent and is not ported.
//
// * Not ported: the Python's `int(name.lstrip('[').rstrip(']'))` in
//   get_child_index. The bytecode cannot parse a string (it cannot even compare
//   two), so the three named members are found with lookup(), and a name like
//   "[3]" is an error. Children are still found by index.
LLDBFMT_SCRIPT_SYNTHETIC(VectorChildren, "^WTF::Vector<.+>$",
    update([](auto valobj) {
        auto buffer = valobj.get_child_member_with_name("m_buffer");
        auto size = valobj.get_child_member_with_name("m_size").get_value_as_unsigned();
        auto data_size = buffer.get_type().get_pointee_type().get_byte_size();
        return state(buffer, size, data_size);
    }),

    num_children([]([[maybe_unused]] auto valobj, [[maybe_unused]] auto buffer, auto size, [[maybe_unused]] auto data_size) {
        return size + 3;
    }),

    child_index([]([[maybe_unused]] auto valobj, [[maybe_unused]] auto buffer, auto size, [[maybe_unused]] auto data_size, auto name) {
        return lookup(name, "m_size", size, "m_capacity", size + 1, "m_buffer", size + 2);
    }),

    child_at_index([](auto valobj, auto buffer, auto size, auto data_size, auto index) {
        return if_(
            index < size, buffer.create_child_at_offset(format("[%d]", index), index * data_size, buffer.get_type().get_pointee_type()),
            index == size, valobj.get_child_member_with_name("m_size"),
            index == size + 1, valobj.get_child_member_with_name("m_capacity"),
            index == size + 2, buffer,
            null);
    }));

////////////////////////////////////////////////////////////////////////////////
// Stand-ins for the WebKit types, with the same member names and nesting.
////////////////////////////////////////////////////////////////////////////////

namespace WebCore {

struct IntPoint {
    int m_x, m_y;
};

struct IntSize {
    int m_width { 0 };
    int m_height { 0 };
};

struct IntRect {
    IntPoint m_location;
    IntSize m_size;
};

} // namespace WebCore

namespace WTF {

template<typename T>
class VectorBufferBase {
protected:
    T* m_buffer { nullptr };
    unsigned m_capacity : 31 { 0 };
    mutable unsigned m_isBorrowed : 1 { 0 };
    unsigned m_size { 0 };
};

template<typename T>
class Vector : VectorBufferBase<T> {
public:
    void append(T value)
    {
        if (this->m_size == this->m_capacity) {
            unsigned newCapacity = this->m_capacity ? this->m_capacity * 2 : 4;
            T* newBuffer = new T[newCapacity];
            for (unsigned i = 0; i < this->m_size; ++i)
                newBuffer[i] = this->m_buffer[i];
            delete[] this->m_buffer;
            this->m_buffer = newBuffer;
            this->m_capacity = newCapacity;
        }
        this->m_buffer[this->m_size++] = value;
    }
};

} // namespace WTF

void stop() { }

int main()
{
    WebCore::IntRect rect { { 10, -20 }, { 300, 400 } };
    WTF::Vector<int> numbers;
    for (int i = 0; i < 5; ++i)
        numbers.append(i * i);
    stop();
    return rect.m_size.m_width;
}

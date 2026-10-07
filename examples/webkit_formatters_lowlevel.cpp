// The same formatters as webkit_formatters.cpp, written with the low-level DSL
// (lldbfmt/lldbfmt.h) instead of the Python-shaped one (lldbfmt/script.h).
// Reimplementations of WebKit's LLDB formatters (Tools/lldb/lldb_webkit.py)
// as embedded bytecode. The original Python is quoted above each one.
//
// The types below only mirror the member layout of the WebKit classes, so this
// builds without WebKit.
//
// Build:  clang++ -std=c++20 -g -Iinclude examples/webkit_formatters_lowlevel.cpp -o build/webkit_formatters_lowlevel
// Try:    lldb -b -o 'b stop' -o run -o up -o 'frame variable' build/webkit_formatters_lowlevel
//
// The synthetic provider needs a recent LLDB (@get_pointee_type, @get_byte_size,
// @create_child_at_offset, dictionaries and Integer literals were added after
// Apple's LLDB 2103). The summaries work with older ones too.
//
// Not ported: looking a child up by a name like "[3]" (see VectorChildren).

#include "lldbfmt/lldbfmt.h"

#include <cstddef>

using namespace lldbfmt;
using namespace lldbfmt::dsl;

// The Python reads integers with GetValueAsSigned/GetValueAsUnsigned. Here the
// members are read with get_value, which yields the same digits as a String:
// LLDB's @fmt (checked against a current llvm-project build, and Apple LLDB
// 2103) formats UInt/Int arguments through a pointer to the stack element,
// printing a bogus address. String and Integer arguments format correctly.
//
// Helpers. In a summary the data stack starts as [valobj], so `0u, pick` copies
// the object being formatted, and each helper leaves one value on top of it.

// obj -> child, for one step of a member path.
template<size_t N>
constexpr auto member(const char (&name)[N])
{
    return sequence(name, selector::get_child_with_name, call);
}

// ( -> value ), where value is the member at the given path read with `read`
// (a selector such as get_value_as_signed).
template<size_t... N>
constexpr auto valueAt(selector read, const char (&...path)[N])
{
    return sequence(0u, pick, member(path)..., read, call);
}

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
// m_location and m_size are IntPoint and IntSize. Each getter reads one `int`
// member, so the whole summary is four member paths and a format string. In
// llvm::formatv syntax a literal '{' is written '{{'.
LLDBFMT_FORMATTER(IntRectSummary, "WebCore::IntRect", 0,
    summary(
        valueAt(selector::get_value, "m_location", "m_x"),
        valueAt(selector::get_value, "m_location", "m_y"),
        valueAt(selector::get_value, "m_size", "m_width"),
        valueAt(selector::get_value, "m_size", "m_height"),
        "{{ x = {0}, y = {1}, width = {2}, height = {3} }", selector::fmt, call,
        return_));

////////////////////////////////////////////////////////////////////////////////
// WTF::Vector<...>
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
// starts with '^' is a regex here too. m_size and m_capacity are read
// directly instead of through an update() step.
LLDBFMT_FORMATTER(VectorSummary, "^WTF::Vector<.+>$", 0,
    summary(
        valueAt(selector::get_value, "m_size"),
        valueAt(selector::get_value, "m_capacity"),
        "{{ size = {0}, capacity = {1} }", selector::fmt, call,
        return_));

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
// * State lives on the data stack, which starts as [valobj]. @update pushes the
//   Python's attributes, so they sit at fixed positions that the getters read
//   with `pick` (which counts from the bottom of the stack):
//       0: valobj    1: buffer    2: size    3: data_size (an Integer)
//   @update's last value, 0u, tells LLDB to refetch children (eRefetch).
//
// * `self.data_size` is an Integer (@get_byte_size), but `index` is a UInt, and
//   `offset = index * self.data_size` needs both to be the same type, and
//   create_child_at_offset needs an Integer. uint_to_integer<>() converts the index.
//
// * @fmt does not remove its arguments from the stack, so the formatted name
//   leaves the Integer behind. The getter keeps everything it needs below that
//   at fixed positions, so the leftover does not matter.
//
// * Not ported: the Python's `int(name.lstrip('[').rstrip(']'))` in
//   get_child_index. The bytecode cannot parse a string (it cannot even compare
//   two), so only the three named members are found by name, via a dictionary,
//   and a name like "[3]" is an error. Children are still found by index.
constexpr auto bufferChild = sequence(0u, pick, member("m_buffer"));

LLDBFMT_FORMATTER(VectorChildren, "^WTF::Vector<.+>$", 0,
    update(
        // self.buffer = valobj.m_buffer
        bufferChild,
        // self.size = valobj.m_size.GetValueAsUnsigned()
        valueAt(selector::get_value_as_unsigned, "m_size"),
        // self.data_size = self.buffer.GetType().GetPointeeType().GetByteSize()
        1u, pick, selector::get_type, call, selector::get_pointee_type, call, selector::get_byte_size, call,
        // Children depend on the size, so refetch.
        0u),

    // return self.size + 3
    get_num_children(2u, pick, 3u, plus),

    // stack: valobj buffer size dataSize name
    get_child_index(
        dict,
        dup, "m_size", 0u, dict_set,
        dup, "m_capacity", 1u, dict_set,
        dup, "m_buffer", 2u, dict_set,
        // name dict -> dict name -> self.size + offset (an error if the name is unknown)
        swap, dict_get,
        2u, pick, plus),

    // stack: valobj buffer size dataSize index
    get_child_at_index(
        // index < size: the element.
        dup, 2u, pick, less,
        if_then(block(
            // The index as an Integer, kept at position 5 for the rest of the block.
            dup, uint_to_integer(),
            // name = '[' + str(index) + ']'
            5u, pick, "[{0}]", selector::fmt, call,
            // buffer, name, offset = index * self.data_size, type
            1u, pick, swap,
            5u, pick, 3u, pick, mul,
            1u, pick, selector::get_type, call, selector::get_pointee_type, call,
            // self.buffer.CreateChildAtOffset(name, offset, self.data_type)
            selector::create_child_at_offset, call,
            return_)),
        // index == size: m_size
        dup, 2u, pick, equal,
        if_then(block(sequence(0u, pick, member("m_size")), return_)),
        // index == size + 1: m_capacity
        dup, 2u, pick, 1u, plus, equal,
        if_then(block(sequence(0u, pick, member("m_capacity")), return_)),
        // index == size + 2: self.buffer
        dup, 2u, pick, 2u, plus, equal,
        if_then(block(1u, pick, return_)),
        // else: None
        null, return_));

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

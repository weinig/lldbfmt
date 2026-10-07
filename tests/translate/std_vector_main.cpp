// A stand-in for libstdc++'s std::vector layout.
#include "std_vector.h"

namespace std {

template<typename T>
struct vector {
    struct impl {
        T* _M_start { nullptr };
        T* _M_finish { nullptr };
        T* _M_end_of_storage { nullptr };
    } _M_impl;
};

}

void stop() { }

int main()
{
    int storage[4] = { 3, 1, 4, 1 };
    std::vector<int> numbers;
    numbers._M_impl = { storage, storage + 4, storage + 4 };
    [[maybe_unused]] std::vector<int> empty;
    stop();
    return storage[0];
}

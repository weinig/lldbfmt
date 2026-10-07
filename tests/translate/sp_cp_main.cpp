// A stand-in for libstdc++'s shared_ptr layout, named so the ".*SP" regexes in
// sp_cp.py match it.
#include "sp_cp.h"

struct Counts {
    int _M_use_count;
};

struct RefCount {
    Counts* _M_pi;
};

struct IntSP {
    int* _M_ptr;
    RefCount _M_refcount;
};

void stop() { }

int main()
{
    int value = 7;
    Counts counts { 2 };
    IntSP pointer { &value, { &counts } };
    stop();
    return *pointer._M_ptr;
}

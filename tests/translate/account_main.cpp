// From lldb/test/API/functionalities/data-formatter/bytecode-synthetic
// (main.cpp and TestBytecodeSynthetic.py), written in the Python-shaped DSL.
// The original is hand-assembled:
//
//   update:             `return eReuse`                          (20 01)
//   get_num_children:   `return 1`                               (20 01)
//   get_child_at_index: `return self.valobj.GetChildAtIndex(idx)` (23 11 60)
//
// The test expects acc to have one child, `username`, and no `password`, and
// the log to show that @update was reused.
#include "lldbfmt/script.h"

using namespace lldbfmt::script;

struct Account {
    char* username;
    char* password;
};

LLDBFMT_SCRIPT_SYNTHETIC(AccountSynthetic, "Account",
    update([]([[maybe_unused]] auto valobj) {
        return state_reuse();
    }),

    num_children([]([[maybe_unused]] auto valobj) {
        return 1;
    }),

    child_at_index([](auto valobj, auto idx) {
        return valobj.get_child_at_index(idx);
    }));

void stop() { }

int main()
{
    Account acc;
    acc.username = const_cast<char*>("jake");
    acc.password = const_cast<char*>("popcorn");
    stop();
    return 0;
}

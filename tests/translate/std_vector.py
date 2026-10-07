# libstdc++'s std::vector synthetic provider, from
# lldb/examples/synthetic/gnu_libstdcpp.py (StdVectorSynthProvider and its
# StdVectorImplementation), flattened into one class. The original caches the
# count lazily and relies on try/except for invalid data; here the count is
# computed in update() after checking that the pointers are valid, and the
# index-by-name parsing in get_child_index (int(name.lstrip("["))) is left out
# because bytecode cannot parse strings.


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand(
        'type synthetic add -x "^std::vector<.+>$" -l std_vector.StdVectorSynthProvider'
    )


class StdVectorSynthProvider:
    def __init__(self, valobj, dict):
        self.valobj = valobj

    def num_children(self):
        return self.count

    def get_child_at_index(self, index):
        if index >= self.count:
            return None
        offset = index * self.data_size
        return self.start.CreateChildAtOffset(
            "[" + str(index) + "]", offset, self.data_type
        )

    def update(self):
        impl = self.valobj.GetChildMemberWithName("_M_impl")
        self.start = impl.GetChildMemberWithName("_M_start")
        self.finish = impl.GetChildMemberWithName("_M_finish")
        self.end = impl.GetChildMemberWithName("_M_end_of_storage")
        self.data_type = self.start.GetType().GetPointeeType()
        self.data_size = self.data_type.GetByteSize()
        # The original also rejects invalid or zero pointers, start >= finish,
        # finish > end and a size that is not a multiple of data_size, in an
        # if/elif chain, and relies on try/except for the rest. Converting the
        # UInt difference to an Integer for the division uses `if` blocks, and a
        # conditional around that would run them inside another conditional's
        # block, and LLDB's interpreter would then drop the code after the
        # outer conditional, so the checks are left out here.
        self.count = (
            self.finish.GetValueAsUnsigned(0) - self.start.GetValueAsUnsigned(0)
        ) // self.data_size
        return False

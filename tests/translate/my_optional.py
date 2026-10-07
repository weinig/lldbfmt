# The MyOptional formatters from lldb's test suite
# (lldb/test/Shell/ScriptInterpreter/Python/Inputs/FormatterBytecode/formatter.py),
# written out in Python: the summary is the commented-out original that the
# hand-assembled bytecode there implements, and the synthetic provider is the
# one described in the comments of MyOptionalSynthProvider.


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand(
        'type synthetic add -l my_optional.MyOptionalSynthProvider -x "^MyOptional<.+>$"'
    )
    debugger.HandleCommand(
        'type summary add -e -F my_optional.MyOptionalSummaryProvider -x "^MyOptional<.+>$"'
    )


def MyOptionalSummaryProvider(valobj, internal_dict):
    storage = valobj.GetChildMemberWithName("Storage")
    if not storage:
        storage = valobj

    hasVal = storage.GetChildMemberWithName("hasVal")
    if not hasVal:
        return "<could not read MyOptional>"

    if hasVal.GetValueAsUnsigned(2) == 0:
        return "None"

    underlying_type = storage.GetType().GetTemplateArgumentType(0)
    val = storage.GetChildMemberWithName("value").Cast(underlying_type)
    if val.summary:
        return val.summary
    return val.GetValue()


class MyOptionalSynthProvider:
    """Provides deref support to llvm::Optional<T>"""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj

    def num_children(self):
        return self.valobj.num_children

    def get_child_index(self, name):
        if name == "$$dereference$$":
            return self.valobj.num_children
        return self.valobj.GetIndexOfChildWithName(name)

    def get_child_at_index(self, index):
        if index < self.valobj.num_children:
            return self.valobj.GetChildAtIndex(index)
        storage = self.valobj.GetChildMemberWithName("Storage")
        if not storage:
            storage = self.valobj
        hasVal = storage.GetChildMemberWithName("hasVal")
        if not hasVal:
            return None
        if hasVal.GetValueAsUnsigned(2) == 0:
            return None
        underlying_type = storage.GetType().GetTemplateArgumentType(0)
        return storage.GetChildMemberWithName("value").Cast(underlying_type)

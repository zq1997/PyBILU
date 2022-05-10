import lldb


def lldb_debugger() -> lldb.SBDebugger:
    return lldb.debugger


def lldb_target() -> lldb.SBTarget:
    return lldb_debugger().GetSelectedTarget()


def lldb_process() -> lldb.SBProcess:
    return lldb_target().GetProcess()


def lldb_thread() -> lldb.SBThread:
    return lldb_process().GetSelectedThread()


def lldb_frame() -> lldb.SBFrame:
    return lldb_thread().GetSelectedFrame()


class Frame:
    class FrameType:
        def __init__(self, outer: 'Frame'):
            self.outer = outer

        def __eq__(self, other):
            f = self.outer.lldb_frame
            if other is INLINE_FRAME:
                return f.is_inlined
            if other is NORMAL_FRAME:
                return f.is_inlined and not f.IsArtificial()

    def __init__(self, lldb_frame: lldb.SBFrame):
        assert lldb_frame.IsValid()
        self.lldb_frame = lldb_frame

    def type(self):
        return Frame.FrameType(self)


NORMAL_FRAME = object()
INLINE_FRAME = object()


def selected_frame():
    return Frame(lldb_frame())


class Command:
    def __init__(self, name, command_class, completer_class=None, prefix=None):
        pass


COMMAND_DATA = None
COMMAND_STACK = None
COMMAND_FILES = None
COMPLETE_NONE = None


class Field:
    def __init__(self, lldb_field: lldb.SBTypeMember, parent: 'Type'):
        assert lldb_field.IsValid()
        self.bitpos = lldb_field.bit_offset
        self.enumval = None
        self.name = lldb_field.name
        self.artificial = False
        self.is_base_class = False
        self.bitsize = lldb_field.bitfield_bit_size if lldb_field.is_bitfield else 0
        self.type = Type(lldb_field.type)
        self.parent_type = parent


TYPE_CODE_PTR = lldb.eTypeClassPointer


class Type:
    def __init__(self, lldb_type: lldb.SBType):
        assert lldb_type.IsValid()
        self.lldb_type = lldb_type
        self.code = lldb_type.GetTypeClass()

    def pointer(self):
        return Type(self.lldb_type.GetPointerType())

    def sizeof(self):
        return self.lldb_type.size

    def fields(self):
        return [Field(f, self) for f in self.lldb_type.fields]

    def unqualified(self):
        return Type(self.lldb_type.GetUnqualifiedType())

    def target(self):
        if self.lldb_type.IsPointerType():
            return Type(self.lldb_type.GetPointeeType())
        if self.lldb_type.IsArrayType():
            return Type(self.lldb_type.GetArrayElementType())
        if self.lldb_type.IsFunctionType():
            return Type(self.lldb_type.GetFunctionReturnType())

    def __str__(self):
        return self.lldb_type.name


class Value:
    def __init__(self, lldb_value: lldb.SBValue):
        assert lldb_value.IsValid()
        self.lldb_value = lldb_value
        self.type = Type(lldb_value.type)
        self.is_optimized_out = False

    def __getitem__(self, name):
        child = self.lldb_value.GetChildMemberWithName(name)
        assert child.IsValid()
        return Value(child)

    def __int__(self):
        bt = self.type.lldb_type.GetBasicType()
        if bt in (lldb.eBasicTypeSignedChar, lldb.eBasicTypeSignedWChar, lldb.eBasicTypeShort,
                  lldb.eBasicTypeInt, lldb.eBasicTypeLong, lldb.eBasicTypeLongLong, lldb.eBasicTypeInt128):
            return self.lldb_value.signed
        return self.lldb_value.unsigned

    def __add__(self, other):
        print(self.lldb_value)
        return Value(self.lldb_value.GetValueForExpressionPath(' + %d' % other))

    def cast(self, to_type: Type):
        return Value(self.lldb_value.Cast(to_type.lldb_type))

    def dereference(self):
        return Value(self.lldb_value.Dereference())

    def string(self):
        return eval(self.lldb_value.summary)

    @property
    def address(self):
        print(self.lldb_value, '\n到\n', self.lldb_value.address_of)
        return Value(self.lldb_value.address_of)


class Symbol:
    def __init__(self, lldb_symbol: lldb.SBSymbol):
        assert lldb_symbol.IsValid()
        self.lldb_symbol = lldb_symbol

    def value(self) -> Value:
        assert self.lldb_symbol.type == lldb.eSymbolTypeData
        return lldb_target().FindFirstGlobalVariable(self.lldb_symbol.name)


def lookup_type(name: str) -> Type:
    lldb_type = lldb.debugger.GetSelectedTarget().FindFirstType(name)
    return Type(lldb_type)


def lookup_global_symbol(name: str) -> Symbol:
    symbols = lldb_target().FindSymbols(name).symbols
    assert len(symbols) == 1
    return Symbol(symbols[0])


def current_objfile():
    return None


pretty_printers = []
error = AssertionError


def pretty_print(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    var: lldb.SBValue = lldb_frame().FindVariable(command)
    if pretty_printers:
        printer = pretty_printers[0](Value(var))
        if printer is not None:
            result.Print(printer.to_string())
            return
    result.SetError('no pretty printer for ' + str(var))


def __lldb_init_module(debugger: lldb.SBDebugger, internal_dict):
    debugger.HandleCommand(f'command script add -o -f {__name__}.pretty_print pp')

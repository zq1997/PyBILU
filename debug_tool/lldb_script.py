import os
import lldb

status_holder = []


class ValueWrapper:
    def __init__(self, v: [lldb.SBValue, str], frame: lldb.SBFrame = None):
        if type(v) is str:
            assert frame is not None
            if frame is None:
                frame = lldb.debugger.GetSelectedTarget().GetProcess().GetSelectedThread().GetSelectedFrame()
            v = frame.FindVariable(v)
            assert v and v.IsValid()
            self._v = v
        self._v: lldb.SBValue = v

    def cast(self, to_type: str, pointer_level=0) -> 'ValueWrapper':
        to_type = self._v.target.FindFirstType(to_type)
        for i in range(pointer_level):
            to_type = to_type.GetPointerType()
        v = self._v.Cast(to_type)
        assert v and v.IsValid()
        self._v = v
        return self

    def __getitem__(self, name) -> 'ValueWrapper':
        child = self._v.GetChildMemberWithName(name)
        if child and child.IsValid():
            return ValueWrapper(child)
        raise AttributeError("Attribute '%s' is not defined" % name)

    def as_signed(self) -> int:
        return self._v.signed

    def as_unsigned(self) -> int:
        return self._v.unsigned

    def as_str(self) -> str:
        return self._v.summary

    def read_data(self, count=1) -> bytes:
        return bytes(self._v.GetPointeeData(0, count).uint8)


def print_obj(debugger: lldb.SBDebugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    v = ValueWrapper(command).cast('PyObject', 1)
    ob_refcnt = v['ob_refcnt'].as_signed()
    tp_name = v['ob_type']['tp_name'].as_str()
    result.AppendMessage(f'addr=0x{v.as_unsigned():x} ref={ob_refcnt} type={tp_name}')


def on_code_loaded(frame: lldb.SBFrame, bp_loc, extra_args, internal_dict):
    py_code = ValueWrapper('py_code', frame)
    obj_file = ValueWrapper('obj_file', frame)
    code_addr = ValueWrapper('code_addr', frame).as_unsigned()

    tmp_file_path = os.path.join(os.path.dirname(__file__), f'code.0x{code_addr:x}')
    try:
        with open(tmp_file_path, 'wb') as f:
            f.write(obj_file['Data'].read_data(obj_file['Length'].as_unsigned()))
        target: lldb.SBTarget = frame.thread.process.target
        mod = target.AddModule(tmp_file_path, None, None)
        target.SetModuleLoadAddress(mod, code_addr)
        print('load module:', mod)
    finally:
        if os.path.isfile(tmp_file_path):
            os.remove(tmp_file_path)


def reload_gdb(debugger: lldb.SBDebugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    debugger.HandleCommand('command script import debug_tool/gdb.py')
    debugger.HandleCommand('command script import debug_tool/ported_script.py')


def __lldb_init_module(debugger: lldb.SBDebugger, internal_dict):
    global status_holder
    if '_holder' in internal_dict:
        status_holder = internal_dict['_holder']
        return
    internal_dict['_holder'] = status_holder

    target: lldb.SBTarget = debugger.GetDummyTarget()
    sp: lldb.SBBreakpoint = target.BreakpointCreateByName('notifyCodeLoaded', 'pynic.so')
    sp.SetScriptCallbackFunction(f'{__name__}.on_code_loaded')
    sp.SetAutoContinue(True)

    debugger.HandleCommand(f'command script add -o -f {__name__}.print_obj py-p')
    debugger.HandleCommand(f'command script add -o -f {__name__}.reload_gdb reload_gdb')

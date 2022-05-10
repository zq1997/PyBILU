import lldb
import shlex
import argparse
import tempfile

temporary_file_holder = None


def get_target(debugger: lldb.SBDebugger) -> lldb.SBTarget:
    return debugger.GetSelectedTarget()


def get_frame(debugger: lldb.SBDebugger) -> lldb.SBFrame:
    process: lldb.SBProcess = get_target(debugger).GetProcess()
    thread: lldb.SBThread = process.GetSelectedThread()
    return thread.GetSelectedFrame()


def get_type(debugger: lldb.SBDebugger, name) -> lldb.SBType:
    return get_target(debugger).FindFirstType(name)


def value_as_string(v):
    return v.sbvalue.summary


def value_as_int(v):
    return v.sbvalue.signed


def revert_lldb_value(value) -> lldb.SBValue:
    return value.sbvalue


def print_obj(debugger: lldb.SBDebugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    v = get_frame(debugger).FindVariable(command)
    if not v.IsValid():
        result.SetError(f'invalid variable: {command}')
        return
    v = lldb.value(v.Cast(get_type(debugger, 'PyObject').GetPointerType()))
    ob_refcnt = value_as_int(v.ob_refcnt)
    tp_name = value_as_string(v.ob_type.tp_name)
    result.AppendMessage(f'addr=0x{value_as_int(v):x} ref={ob_refcnt} type={tp_name}')


def on_code_loaded(frame: lldb.SBFrame, bp_loc, extra_args, internal_dict):
    pycode: lldb.SBValue = frame.FindVariable('pycode')
    obj_file: lldb.SBValue = frame.FindVariable('obj_file')
    code_addr: lldb.SBValue = frame.FindVariable('code_addr').unsigned
    # temp = tempfile.NamedTemporaryFile(prefix=f'code.0x{loaded_addr:x}.', dir='./')
    # print(temp.name)
    #
    # # v: lldb.SBData = obj_file.GetPointeeData(0, obj_file_size)
    # temp.write(b'v.ReadRawData()')
    # temp.flush()
    # temporary_file_holder.append(temp)
    # # target: lldb.SBTarget = frame.thread.process.target
    print(code_addr)


def __lldb_init_module(debugger: lldb.SBDebugger, internal_dict):
    global temporary_file_holder
    if '_holder' in internal_dict:
        temporary_file_holder = internal_dict['_holder']
        return
    temporary_file_holder = internal_dict['_holder'] = []

    target: lldb.SBTarget = debugger.GetDummyTarget()
    sp: lldb.SBBreakpoint = target.BreakpointCreateByName('notifyCodeLoaded', 'pynic.so')
    sp.SetScriptCallbackFunction(f'{__name__}.on_code_loaded')
    sp.SetAutoContinue(True)

    debugger.HandleCommand(f'command script add -o -f {__name__}.print_obj py-p')

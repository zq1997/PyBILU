import os
import importlib.util

import lldb

import gdb_script
import gdb


def on_code_loaded(frame: lldb.SBFrame, bp_loc, extra_args, internal_dict):
    py_code = frame.FindVariable('py_code')
    code_addr = frame.FindVariable('code_addr').unsigned
    obj_file = frame.FindVariable('obj_file')

    tmp_file_path = os.path.join(os.path.dirname(__file__), f'code.0x{code_addr:x}')
    try:
        with open(tmp_file_path, 'wb') as f:
            length = obj_file.GetChildMemberWithName('Length').unsigned
            data = bytes(obj_file.GetChildMemberWithName('Data').GetPointeeData(0, length).uint8)
            f.write(data)
        target: lldb.SBTarget = frame.thread.process.target
        mod = target.AddModule(tmp_file_path, None, None)
        target.SetModuleLoadAddress(mod, code_addr)
        print('jit module:', mod)
    finally:
        if os.path.isfile(tmp_file_path):
            os.remove(tmp_file_path)


def cpython_var_to_str(var: gdb.Value):
    assert len(gdb.pretty_printers) == 1
    printer = gdb.pretty_printers[0](var)
    if printer is not None:
        return printer.to_string()
    return None


def pretty_print(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    var = gdb.get_lldb_frame().EvaluateExpression(command)
    if (var_str := cpython_var_to_str(gdb.Value(var))) is not None:
        result.Print(var_str)
        return
    result.SetError('no pretty printer for ' + str(var))


def pretty_print_force(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    var = gdb.get_lldb_frame().EvaluateExpression(command)
    gdb_var = gdb.Value(var).cast(gdb.lookup_type('PyObject').pointer())
    gdb_var_deref = gdb_var.dereference()
    addr = int(gdb_var_deref.address)
    ob_refcnt = int(gdb_var_deref['ob_refcnt'])
    tp_name = gdb_var_deref['ob_type'].dereference()['tp_name'].string()
    result.Print(f'{tp_name}@0x{addr:x} ({ob_refcnt} refs)\t{cpython_var_to_str(gdb_var)}')


def __lldb_init_module(debugger: lldb.SBDebugger, internal_dict):
    if '_already_loaded' in internal_dict:
        importlib.reload(gdb)
        importlib.reload(gdb_script)
    else:
        internal_dict['_already_loaded'] = True
        target: lldb.SBTarget = debugger.GetDummyTarget()
        sp: lldb.SBBreakpoint = target.BreakpointCreateByName('notifyCodeLoaded', 'pynic.so')
        sp.SetScriptCallbackFunction(f'{__name__}.on_code_loaded')
        sp.SetAutoContinue(True)

        debugger.HandleCommand('command script import debug_tool/gdb_script.py')
        debugger.HandleCommand(f'command script add -o -f {__name__}.pretty_print pp')
        debugger.HandleCommand(f'command script add -o -f {__name__}.pretty_print_force pp-force')

    gdb_script.chr = lambda x: chr(int(x))
    gdb_script.EVALFRAME = '_PyEval_EvalFrame'

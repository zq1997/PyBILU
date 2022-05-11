import os
import importlib.util

import lldb

import gdb_script
import gdb


def on_code_loaded(frame: lldb.SBFrame, bp_loc, extra_args, internal_dict):
    py_code = gdb_script.PyCodeObjectPtr(gdb.Value(frame.FindVariable('py_code')))
    code_addr = frame.FindVariable('code_addr').unsigned
    obj_file = frame.FindVariable('obj_file')

    co_filename = gdb_script.PyObjectPtr.from_pyobject_ptr(py_code.field('co_filename')).proxyval(None)
    co_name = gdb_script.PyObjectPtr.from_pyobject_ptr(py_code.field('co_name')).proxyval(None)
    co_firstlineno = int(py_code.field('co_firstlineno'))
    assert os.path.isfile(co_filename)
    cache_filename = '%s.%d.%s' % (co_filename, co_firstlineno, co_name)
    obj_cache_filename = cache_filename + '.obj'

    with open(obj_cache_filename, 'wb') as f:
        length = obj_file.GetChildMemberWithName('Length').unsigned
        data = bytes(obj_file.GetChildMemberWithName('Data').GetPointeeData(0, length).uint8)
        f.write(data)
    target: lldb.SBTarget = frame.thread.process.target
    mod = target.AddModule(obj_cache_filename, None, None)
    target.SetModuleLoadAddress(mod, code_addr)
    print('jit module loaded:', mod)


def pretty_print(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    var = gdb.get_lldb_frame().EvaluateExpression(command)
    printer = gdb_script.pretty_printer_lookup(gdb.Value(var))
    if printer is not None:
        result.Print(printer.to_string())
        return
    result.SetError('no pretty printer for ' + str(var))


def print_ref(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    var = gdb.get_lldb_frame().EvaluateExpression(command)
    cast_type = gdb_script.PyObjectPtr.get_gdb_type()
    ob_refcnt = int(gdb_script.PyObjectPtr(gdb.Value(var), cast_type).field('ob_refcnt'))
    result.Print(f'{ob_refcnt} refs')


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
        debugger.HandleCommand(f'command script add -o -f {__name__}.print_ref py-ref')

    gdb_script.chr = lambda x: chr(int(x))
    gdb_script.EVALFRAME = '_PyEval_EvalFrame'

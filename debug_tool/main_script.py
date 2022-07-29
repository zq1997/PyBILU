import os
import re
import importlib.util

import lldb

import gdb_script
import gdb


def on_code_loaded(frame: lldb.SBFrame, bp_loc, extra_args, internal_dict):
    py_code = gdb_script.PyCodeObjectPtr.from_pyobject_ptr(gdb.Value(frame.FindVariable('py_code')))
    code_addr = frame.FindVariable('code_addr').unsigned

    co_filename = gdb_script.PyObjectPtr.from_pyobject_ptr(py_code.field('co_filename')).proxyval(None)
    co_name = gdb_script.PyObjectPtr.from_pyobject_ptr(py_code.field('co_name')).proxyval(None)
    co_firstlineno = int(py_code.field('co_firstlineno'))

    py_dir, py_file = os.path.split(co_filename)
    obj_file = os.path.join(py_dir, '__pycache__', '%s.%s.o' % (py_file, co_name))
    assert os.path.isfile(co_filename) and os.path.isfile(obj_file)

    target: lldb.SBTarget = frame.thread.process.target
    mod = target.AddModule(obj_file, None, None)
    target.SetModuleLoadAddress(mod, code_addr)
    print('jit module loaded:', mod)


def pretty_print(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    var = gdb.get_lldb_frame().EvaluateExpression(command)
    printer = gdb_script.PyObjectPtrPrinter(gdb.Value(var))
    if printer is not None:
        result.Print(printer.to_string() + '\n')
        return
    result.SetError('no pretty printer for ' + str(var))


def print_ref(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    var = gdb.get_lldb_frame().EvaluateExpression(command)
    cast_type = gdb_script.PyObjectPtr.get_gdb_type()
    ob_refcnt = int(gdb_script.PyObjectPtr(gdb.Value(var), cast_type).field('ob_refcnt'))
    result.Print(f'{ob_refcnt} refs\n')


def print_stack(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    frame = gdb_script.Frame.get_selected_frame()
    while frame and not frame.is_evalframe():
        frame = frame.older()
    if frame:
        frame = frame.get_pyop()
    else:
        result.SetError('no frame')
        return
    stack_size = gdb_script.PyCodeObjectPtr.from_pyobject_ptr(frame.field('f_code')).field('co_stacksize')
    limit = gdb_script.int_from_int(stack_size)
    limit = min(limit, int(command)) if command else limit
    stack = frame.field('f_valuestack')
    stack_address = gdb_script.int_from_int(stack)
    cast_type = gdb_script.PyObjectPtr.get_gdb_type()
    for i in range(limit):
        element = stack[i]
        try:
            ref = int(gdb_script.PyObjectPtr(element, cast_type).field('ob_refcnt'))
            assert ref > 0
            text = gdb_script.PyObjectPtr.from_pyobject_ptr(element).get_truncated_repr(128)
        except (gdb_script.NullPyObjectPtr, AssertionError):
            result.AppendMessage('%2d ----' % i)
        else:
            result.AppendMessage('%2d %016x [%4d] %s\n' % (i, stack_address + 8 * i, ref, text))


def __lldb_init_module(debugger: lldb.SBDebugger, internal_dict):
    if '_already_loaded' in internal_dict:
        importlib.reload(gdb)
        importlib.reload(gdb_script)
    else:
        internal_dict['_already_loaded'] = True
        target: lldb.SBTarget = debugger.GetDummyTarget()
        sp: lldb.SBBreakpoint = target.BreakpointCreateByName('notifyCodeLoaded', 'compyler.so')
        sp.SetScriptCallbackFunction(f'{__name__}.on_code_loaded')
        sp.SetAutoContinue(True)

        debugger.HandleCommand('command script import debug_tool/gdb_script.py')
        debugger.HandleCommand(f'command script add -o -f {__name__}.pretty_print pp')
        debugger.HandleCommand(f'command script add -o -f {__name__}.print_ref py-ref')
        debugger.HandleCommand(f'command script add -o -f {__name__}.print_stack py-stack')

    gdb_script.chr = lambda x: chr(int(x))
    gdb_script.EVALFRAME = '_PyEval_EvalFrame'

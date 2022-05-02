#include <iostream>

using namespace std;

#include <Python.h>
#include <internal/pycore_pyerrors.h>

#include "translator.h"

static unique_ptr<Compiler> compiler;
static unique_ptr<Translator> translator;
static Py_ssize_t code_extra_index;


// PyObject *vectorcall(PyObject *callable, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
//     assert(PyFunction_Check(callable));
//     auto func = reinterpret_cast<PyFunctionObject *>(callable);
//     // argument check here
//     // auto nargs = PyVectorcall_NARGS(nargsf);
// }

PyObject *eval_func(PyThreadState *tstate, PyFrameObject *f, int throwflag) {
    // TODO: manually implement set/get extra
    void *compiled_code;
    if (_PyCode_GetExtra(reinterpret_cast<PyObject *>(f->f_code), code_extra_index, &compiled_code) == -1) {
        return nullptr;
    }
    if (!compiled_code) {
        return _PyEval_EvalFrameDefault(tstate, f, throwflag);
    }
    // Strictly speaking, converting void* to a function pointer is undefined behavior in C++
    auto jit_callee = reinterpret_cast<TranslatedFunctionType *>(compiled_code);
    // TODO: support generator and throwflag
    assert(!throwflag);

    auto prev_cframe = tstate->cframe;
    ExtendedCFrame cframe;
    cframe.use_tracing = prev_cframe->use_tracing;
    cframe.previous = prev_cframe;
    tstate->cframe = &cframe;
    tstate->frame = f;
    PyObject *result;
    if (!setjmp(cframe.frame_jmp_buf)) {
        result = jit_callee(&symbol_addresses[0], f, 0);
        // TODO: 为啥return的时候不要清理stack
    } else {
        assert(_PyErr_Occurred(tstate));
        PyTraceBack_Here(f);
        assert(!tstate->c_tracefunc); // 暂时的
        f->f_state = FRAME_UNWINDING;

        while (f->f_iblock > 0) {
            /* Pop the current block. */
            PyTryBlock *b = &f->f_blockstack[--f->f_iblock];

            if (b->b_type == EXCEPT_HANDLER) {
                PyObject *type, *value, *traceback;
                _PyErr_StackItem *exc_info;
                while (f->f_stackdepth > b->b_level + 3) {
                    Py_XDECREF(f->f_valuestack[--f->f_stackdepth]);
                }
                exc_info = tstate->exc_info;
                type = exc_info->exc_type;
                value = exc_info->exc_value;
                traceback = exc_info->exc_traceback;
                exc_info->exc_type = f->f_valuestack[--f->f_stackdepth];
                exc_info->exc_value = f->f_valuestack[--f->f_stackdepth];
                exc_info->exc_traceback = f->f_valuestack[--f->f_stackdepth];
                Py_XDECREF(type);
                Py_XDECREF(value);
                Py_XDECREF(traceback);
                continue;
            }
            while (f->f_stackdepth > b->b_level) {
                PyObject *v = f->f_valuestack[--f->f_stackdepth];
                Py_XDECREF(v);
            }
            assert(b->b_type == SETUP_FINALLY);
            if (b->b_type == SETUP_FINALLY) {
                PyObject *exc, *val, *tb;
                int handler = b->b_handler;
                _PyErr_StackItem *exc_info = tstate->exc_info;
                /* Beware, this invalidates all b->b_* fields */
                PyFrame_BlockSetup(f, EXCEPT_HANDLER, f->f_lasti, f->f_stackdepth);
                f->f_valuestack[f->f_stackdepth++] = exc_info->exc_traceback;
                f->f_valuestack[f->f_stackdepth++] = exc_info->exc_value;
                if (exc_info->exc_type) {
                    f->f_valuestack[f->f_stackdepth++] =  exc_info->exc_type;
                } else {
                    Py_INCREF(Py_None);
                    f->f_valuestack[f->f_stackdepth++] = Py_None;
                }
                _PyErr_Fetch(tstate, &exc, &val, &tb);
                /* Make the raw exception data
                   available to the handler,
                   so a program can emulate the
                   Python main loop. */
                _PyErr_NormalizeException(tstate, &exc, &val, &tb);
                if (tb) {
                    PyException_SetTraceback(val, tb);
                } else {
                    PyException_SetTraceback(val, Py_None);
                }
                Py_INCREF(exc);
                exc_info->exc_type = exc;
                Py_INCREF(val);
                exc_info->exc_value = val;
                exc_info->exc_traceback = tb;
                if (!tb) {
                    tb = Py_None;
                }
                Py_INCREF(tb);
                f->f_valuestack[f->f_stackdepth++] = tb;
                f->f_valuestack[f->f_stackdepth++] = val;
                f->f_valuestack[f->f_stackdepth++] = exc;
                /* Resume normal execution */
                f->f_state = FRAME_EXECUTING;
                result = jit_callee(&symbol_addresses[0], f, handler);
            }
        }

        while (f->f_stackdepth) {
            PyObject *v = f->f_valuestack[--f->f_stackdepth];
            Py_XDECREF(v);
        }
        f->f_stackdepth = 0;
        f->f_state = FRAME_RAISED;
        return nullptr;
    }
    tstate->cframe = prev_cframe;
    tstate->frame = f->f_back;
    return result;
}

void freeExtra(void *extra) {
    // 没有实现内存管理
}

PyObject *apply(PyObject *, PyObject *maybe_func) {
    if (!PyFunction_Check(maybe_func)) {
        PyErr_SetString(PyExc_TypeError, "bad argument type");
        return nullptr;
    }
    auto func = reinterpret_cast<PyFunctionObject *>(maybe_func);
    void *compiled_func;
    try {
        compiled_func = (*translator)(*compiler, reinterpret_cast<PyCodeObject *>(func->func_code));
    } catch (runtime_error &err) {
        PyErr_SetString(PyExc_RuntimeError, err.what());
        return nullptr;
    }
    _PyCode_SetExtra(func->func_code, code_extra_index, compiled_func);
    return Py_NewRef(func);
}

PyMODINIT_FUNC PyInit_pynic() {
    try {
        compiler = make_unique<Compiler>();
        translator = make_unique<Translator>(compiler->createDataLayout());
    } catch (runtime_error &err) {
        PyErr_SetString(PyExc_RuntimeError, err.what());
        return nullptr;
    }

    static PyMethodDef meth_def[] = {{"apply", apply, METH_O}, {}};
    static PyModuleDef mod_def = {
            PyModuleDef_HEAD_INIT,
            "pynic",
            "pynic: Python to Native Code",
            -1,
            meth_def
    };
    code_extra_index = _PyEval_RequestCodeExtraIndex(freeExtra);
    if (code_extra_index < 0) {
        PyErr_SetString(PyExc_RuntimeError, "failed to setup");
        return nullptr;
    }
    auto this_mod = PyModule_Create(&mod_def);
    if (this_mod) {
        _PyInterpreterState_SetEvalFrameFunc(PyInterpreterState_Get(), eval_func);
    }
    return PyModule_Create(&mod_def);
}

#include <Python.h>
#include <internal/pycore_pyerrors.h>

#include "compile_unit.h"

using namespace std;

static unique_ptr<Translator> translator;
static Py_ssize_t code_extra_index;


PyObject *eval_func(PyThreadState *tstate, PyFrameObject *f, int throwflag) {
    // TODO: manually implement set/get extra
    CompileUnit::TranslatedResult *compiled_result;
    if (_PyCode_GetExtra(reinterpret_cast<PyObject *>(f->f_code),
            code_extra_index,
            reinterpret_cast<void **>(&compiled_result)) == -1) {
        return nullptr;
    }
    if (!compiled_result) {
        return _PyEval_EvalFrameDefault(tstate, f, throwflag);
    }
    // TODO: support generator and throwflag
    assert(!throwflag);

    auto prev_cframe = tstate->cframe;
    ExtendedCFrame cframe;
    cframe.use_tracing = prev_cframe->use_tracing;
    cframe.previous = prev_cframe;
    tstate->cframe = &cframe;
    tstate->frame = f;
    PyObject *result;
    if (!setjmp(cframe.frame_jmp_buf)) [[likely]] {
        result = (*compiled_result)(&symbol_addresses[0], f, ptrdiff_t{0});
        f->f_stackdepth = compiled_result->sp_map[f->f_lasti];
        // TODO: 为啥return的时候不要清理stack
    } else {
        f->f_stackdepth = compiled_result->sp_map[f->f_lasti];
        assert(_PyErr_Occurred(tstate));
        PyTraceBack_Here(f);
        assert(!tstate->c_tracefunc); // 暂时的
        f->f_state = FRAME_UNWINDING;
        ptrdiff_t handler = -1;

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
                handler = b->b_handler;
                _PyErr_StackItem *exc_info = tstate->exc_info;
                /* Beware, this invalidates all b->b_* fields */
                PyFrame_BlockSetup(f, EXCEPT_HANDLER, f->f_lasti, f->f_stackdepth);
                f->f_valuestack[f->f_stackdepth++] = exc_info->exc_traceback;
                f->f_valuestack[f->f_stackdepth++] = exc_info->exc_value;
                if (exc_info->exc_type) {
                    f->f_valuestack[f->f_stackdepth++] = exc_info->exc_type;
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
                break;
            }
        }

        if (handler > 0) {
            result = (*compiled_result)(&symbol_addresses[0], f, handler);
            f->f_stackdepth = compiled_result->sp_map[f->f_lasti];
        } else {
            while (f->f_stackdepth) {
                PyObject *v = f->f_valuestack[--f->f_stackdepth];
                Py_XDECREF(v);
            }
            f->f_state = FRAME_RAISED;
            return nullptr;
        }
    }
    tstate->cframe = prev_cframe;
    tstate->frame = f->f_back;
    assert(result);
    return result;
}

void freeExtra(void *result) {
    auto result_ = reinterpret_cast< CompileUnit::TranslatedResult *>(result);
    unloadCode(result_->mem_block);
    delete result_;
}

PyObject *apply(PyObject *, PyObject *maybe_func) {
    if (!PyFunction_Check(maybe_func)) {
        PyErr_SetString(PyExc_TypeError, "bad argument type");
        return nullptr;
    }
    auto func = reinterpret_cast<PyFunctionObject *>(maybe_func);
    CompileUnit::TranslatedResult *result;
    try {
        result = CompileUnit::emit(*translator, func->func_code);
    } catch (runtime_error &err) {
        PyErr_SetString(PyExc_RuntimeError, err.what());
        return nullptr;
    } catch (bad_exception &) {
        return nullptr;
    }
    _PyCode_SetExtra(func->func_code, code_extra_index, result);
    return Py_NewRef(func);
}

PyMODINIT_FUNC PyInit_compyler() {
    try {
        translator = make_unique<Translator>();
    } catch (runtime_error &err) {
        PyErr_SetString(PyExc_RuntimeError, err.what());
        return nullptr;
    }

    static PyMethodDef meth_def[] = {{"apply", apply, METH_O}, {}};
    static PyModuleDef mod_def = {
            PyModuleDef_HEAD_INIT,
            "comPyler",
            "A lightweight Just-In-Time compiler for Python",
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

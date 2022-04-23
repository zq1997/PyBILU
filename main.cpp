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

PyObject *eval_func(PyThreadState *tstate, PyFrameObject *frame, int throwflag) {
    // TODO: manually implement set/get extra
    void *compiled_code;
    if (_PyCode_GetExtra(reinterpret_cast<PyObject *>(frame->f_code), code_extra_index, &compiled_code) == -1) {
        return nullptr;
    }
    if (!compiled_code) {
        return _PyEval_EvalFrameDefault(tstate, frame, throwflag);
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
    if (!setjmp(cframe.frame_jmp_buf)) {
        return jit_callee(&symbol_addresses[0], frame);
    } else {
        assert(_PyErr_Occurred(tstate));
        PyTraceBack_Here(frame);
        auto nlocals = frame->f_code->co_nlocals;
        auto ncells = PyTuple_GET_SIZE(frame->f_code->co_cellvars);
        auto nfrees = PyTuple_GET_SIZE(frame->f_code->co_freevars);
        auto stack = &frame->f_localsplus[nlocals + ncells + nfrees];
        for (auto i : Range(frame->f_stackdepth)) {
            Py_XDECREF(stack[i]);
        }
        return nullptr;
    }
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

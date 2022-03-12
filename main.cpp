#include <iostream>

using namespace std;

#include <Python.h>
#include <frameobject.h>

#include "JIT.h"

static unique_ptr<Compiler> compiler;
static unique_ptr<Translator> translator;
static Py_ssize_t code_extra_index;

PyObject *vectorcall(PyObject *callable, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    assert(PyFunction_Check(callable));
    auto func = reinterpret_cast<PyFunctionObject *>(callable);
    // argument check here
    // auto nargs = PyVectorcall_NARGS(nargsf);
    PyObject *(*jit_func)(decltype(&global_symbol_table), PyObject *const *);
    _PyCode_GetExtra(func->func_code, code_extra_index, reinterpret_cast<void **>(&jit_func));
    return jit_func(&global_symbol_table, args);
}

PyObject *eval_func(PyThreadState *tstate, PyFrameObject *frame, int throwflag) {
    // TODO: manually implement set/get extra
    void *jit_func;
    if (_PyCode_GetExtra(reinterpret_cast<PyObject *>(frame->f_code), code_extra_index, &jit_func) == -1) {
        return nullptr;
    }
    if (!jit_func) {
        return _PyEval_EvalFrameDefault(tstate, frame, throwflag);
    }
    // TODO: support generator and throwflag
    assert(!throwflag);
    return reinterpret_cast<PyObject *(*)(decltype(&global_symbol_table), PyObject **, PyObject **)>(jit_func)(
            &global_symbol_table, frame->f_localsplus, &PyTuple_GET_ITEM(frame->f_code->co_consts, 0)
    );
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
    // func->vectorcall = vectorcall;
    return Py_NewRef(func);
}

PyMODINIT_FUNC PyInit_pynic() {
    try {
        compiler = make_unique<Compiler>();
        translator = make_unique<Translator>();
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

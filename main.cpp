#include <iostream>

using namespace std;

#include <Python.h>

#include "JIT.h"

static unique_ptr<MyJIT> jit;
static Py_ssize_t extra_index;

PyObject *vectorcall(PyObject *callable, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    assert(PyFunction_Check(callable));
    auto func = reinterpret_cast<PyFunctionObject *>(callable);
    // argument check here
    // auto nargs = PyVectorcall_NARGS(nargsf);
    PyObject *(*jit_func)(PyObject *const *);
    _PyCode_GetExtra(func->func_code, extra_index, reinterpret_cast<void **>(&jit_func));
    return jit_func(args);
}

void free_extra(void *extra) {
    // auto code_extra = reinterpret_cast<CodeExtra *>(extra);
    // cout << "free " << code_extra << endl;
}

PyObject *apply(PyObject *, PyObject *maybe_func) {
    if (!PyFunction_Check(maybe_func)) {
        PyErr_SetString(PyExc_TypeError, "bad argument type");
        return nullptr;
    }
    auto func = reinterpret_cast<PyFunctionObject *>(maybe_func);
    _PyCode_SetExtra(func->func_code, extra_index, jit->to_machine_code(func->func_code));
    func->vectorcall = vectorcall;
    return Py_NewRef(func);
}

PyMODINIT_FUNC
PyInit_pybilu() {
    MyJIT::init();
    jit = make_unique<MyJIT>();

    static PyMethodDef meth_def[] = {
            {"apply", apply, METH_O},
            {}
    };
    static PyModuleDef mod_def = {
            PyModuleDef_HEAD_INIT,
            "pybilu",
            "PyBILU: Python Bytecode Interpretation Loop Unroller",
            -1,
            meth_def
    };
    extra_index = _PyEval_RequestCodeExtraIndex(free_extra);
    if (extra_index < 0) {
        PyErr_SetString(PyExc_TypeError, "failed to setup");
        return nullptr;
    }
    return PyModule_Create(&mod_def);
}

#include <iostream>

using namespace std;

#include <Python.h>

#include "JIT.h"

extern const struct CallTable call_table;
static unique_ptr<MyJIT> jit;
static Py_ssize_t code_extra_index;

PyObject *vectorcall(PyObject *callable, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    assert(PyFunction_Check(callable));
    auto func = reinterpret_cast<PyFunctionObject *>(callable);
    // argument check here
    // auto nargs = PyVectorcall_NARGS(nargsf);
    PyObject *(*jit_func)(decltype(&call_table), PyObject *const *);
    _PyCode_GetExtra(func->func_code, code_extra_index, reinterpret_cast<void **>(&jit_func));
    return jit_func(&call_table, args);
}

void freeExtra(void *extra) {
    // auto code_extra = reinterpret_cast<CodeExtra *>(extra);
    // cout << "free " << code_extra << endl;
}

PyObject *apply(PyObject *, PyObject *maybe_func) {
    if (!PyFunction_Check(maybe_func)) {
        PyErr_SetString(PyExc_TypeError, "bad argument type");
        return nullptr;
    }
    auto func = reinterpret_cast<PyFunctionObject *>(maybe_func);
    void *compiled_func;
    try {
        compiled_func = jit->compile(func->func_code);
    } catch (runtime_error &err) {
        PyErr_SetString(PyExc_RuntimeError, err.what());
        return nullptr;
    }
    _PyCode_SetExtra(func->func_code, code_extra_index, compiled_func);
    func->vectorcall = vectorcall;
    return Py_NewRef(func);
}

PyMODINIT_FUNC PyInit_pybilu() {
    try {
        MyJIT::init();
        jit = make_unique<MyJIT>();
    } catch (runtime_error &err) {
        PyErr_SetString(PyExc_RuntimeError, err.what());
        return nullptr;
    }

    static PyMethodDef meth_def[] = {{"apply", apply, METH_O}, {}};
    static PyModuleDef mod_def = {
            PyModuleDef_HEAD_INIT,
            "pybilu",
            "PyBILU: Python Bytecode Interpretation Loop Unroller",
            -1,
            meth_def
    };
    code_extra_index = _PyEval_RequestCodeExtraIndex(freeExtra);
    if (code_extra_index < 0) {
        PyErr_SetString(PyExc_TypeError, "failed to setup");
        return nullptr;
    }
    return PyModule_Create(&mod_def);
}

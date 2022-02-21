#include <iostream>

using namespace std;

#include <Python.h>

void *run(PyCodeObject *);

PyObject *vectorcall(PyObject *callable, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    assert(PyFunction_Check(callable));
    auto func = reinterpret_cast<PyFunctionObject *>(callable);
    PyObject *(*jit_func)();
    _PyCode_GetExtra(func->func_code, 0, reinterpret_cast<void **>(&jit_func));
    return jit_func();
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
    _PyCode_SetExtra(func->func_code, 0, run(reinterpret_cast<PyCodeObject *>(func->func_code)));
    func->vectorcall = vectorcall;
    return Py_NewRef(func);
}

PyMODINIT_FUNC
PyInit_pybilu() {
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
    _PyEval_RequestCodeExtraIndex(free_extra);
    return PyModule_Create(&mod_def);
}

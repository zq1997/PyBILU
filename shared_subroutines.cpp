#include "shared_symbols.h"

PyObject *calcUnaryNot(PyObject *value) {
    int err = PyObject_IsTrue(value);
    if (err == 0) {
        Py_INCREF(Py_True);
        return Py_True;
    } else if (err > 0) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    return nullptr;
}

PyObject *calcBinaryPower(PyObject *base, PyObject *exp) {
    return PyNumber_Power(base, exp, Py_None);
}

PyObject *calcInPlacePower(PyObject *base, PyObject *exp) {
    return PyNumber_InPlacePower(base, exp, Py_None);
}


PyObject *unwindFrame(PyObject **stack, ptrdiff_t stack_height) {
    while (stack_height--) {
        Py_DECREF(stack[stack_height]);
    }
    return nullptr;
}

#include "shared_symbols.h"

PyObject *calcBinaryPower(PyObject *base, PyObject *exp) {
    return PyNumber_Power(base, exp, Py_None);
}
PyObject *calcInPlacePower(PyObject *base, PyObject *exp) {
    return PyNumber_Power(base, exp, Py_None);
}


PyObject *unwindFrame(PyObject **stack, ptrdiff_t stack_height) {
    while (stack_height--) {
        Py_DECREF(stack[stack_height]);
    }
    return nullptr;
}

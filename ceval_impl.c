#include <Python.h>

#define GETLOCAL(i)     (fastlocals[i])
#define PUSH(v)     (*stack_pointer++ = (v))
#define SET_TOP(v)        (stack_pointer[-1] = (v))
#define TOP()             (stack_pointer[-1])
#define POP()       (*--stack_pointer)

_Noreturn static void goto_error() {
    for (;;);
}

void handle_LOAD_FAST(PyObject **stack_pointer, int oparg, PyObject **fastlocals) {
    PyObject *value = GETLOCAL(oparg);
    if (value == NULL) {
        goto_error();
    }
    Py_INCREF(value);
    PUSH(value);
}


void handle_BINARY_MULTIPLY(PyObject **stack_pointer, int oparg) {
    PyObject *right = POP();
    PyObject *left = TOP();
    PyObject *res = PyNumber_Multiply(left, right);
    Py_DECREF(left);
    Py_DECREF(right);
    SET_TOP(res);
    if (res == NULL) {
        goto_error();
    }
}

PyObject *handle_RETURN_VALUE(PyObject **stack_pointer) {
    return POP();
}

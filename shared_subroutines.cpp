#include "shared_symbols.h"

PyObject *unwindFrame(PyObject **stack, ptrdiff_t stack_height) {
    while (stack_height--) {
        Py_DECREF(stack[stack_height]);
    }
    return nullptr;
}

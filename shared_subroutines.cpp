#include "shared_symbols.h"

#define Py_BUILD_CORE

#include <internal/pycore_pyerrors.h>

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


// static void raiseUnboundLocalError(PyThreadState *tstate, PyObject *name_tuple, Py_ssize_t index) {
//     auto name = PyTuple_GET_ITEM(name_tuple, index);
//     auto name_u8 = PyUnicode_AsUTF8(name);
//     if (name_u8) {
//         return;
//     }
//     _PyErr_Format(tstate, PyExc_UnboundLocalError,
//             "local variable '%.200s' referenced before assignment",
//             name_u8);
// }

static void raiseFreeError(PyThreadState *tstate, PyObject *name) {
    auto name_u8 = PyUnicode_AsUTF8(name);
    if (name_u8) {
        return;
    }
    _PyErr_Format(tstate, PyExc_NameError,
            "free variable '%.200s' referenced before assignment in enclosing scope", name_u8);

    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    if (PyErr_GivenExceptionMatches(value, PyExc_NameError)) {
        PyObject_SetAttrString(value, "name", name);
    }
    PyErr_Restore(type, value, traceback);
}

PyObject *handle_LOAD_CLASSDEREF(SimplePyFrame *f, PyOparg oparg) {
    auto locals = f->locals;
    assert(locals);
    auto co = f->code;
    auto free_index = oparg - PyTuple_GET_SIZE(co->co_cellvars);
    assert(free_index >= 0 && free_index < PyTuple_GET_SIZE(co->co_freevars));
    auto name = PyTuple_GET_ITEM(co->co_freevars, free_index);
    if (PyDict_CheckExact(locals)) {
        auto value = PyDict_GetItemWithError(locals, name);
        if (value) {
            Py_INCREF(value);
            return value;
        } else if (_PyErr_Occurred(f->tstate)) {
            return nullptr;
        }
    } else {
        auto value = PyObject_GetItem(locals, name);
        if (value) {
            return value;
        } else {
            if (!_PyErr_ExceptionMatches(f->tstate, PyExc_KeyError)) {
                return nullptr;
            }
            _PyErr_Clear(f->tstate);
        }
    }
    auto value = PyCell_GET(f->localsplus[f->code->co_nlocals + oparg]);
    if (value) {
        Py_INCREF(value);
        return value;
    }
    raiseFreeError(f->tstate, name);
    return nullptr;
}

PyObject *unwindFrame(PyObject **stack, ptrdiff_t stack_height) {
    while (stack_height--) {
        Py_DECREF(stack[stack_height]);
    }
    return nullptr;
}


const auto symbol_names{std::apply(
        [](auto &&... x) noexcept { return std::array{x.second ...}; },
        external_symbols
)};
const auto symbol_addresses{std::apply(
        [](auto &&... x) noexcept { return std::array{reinterpret_cast<FunctionPointer>(x.first) ...}; },
        external_symbols
)};

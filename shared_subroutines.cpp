#include "shared_symbols.h"

#define Py_BUILD_CORE

#include <internal/pycore_code.h>
#include <internal/pycore_pyerrors.h>

// TODO: 在想，能不能设计俩版本，decref在这里实现

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

static void raiseNameError(PyThreadState *tstate, PyObject *name,
        const char *format_str = "name '%.200s' is not defined") {
    auto name_u8 = PyUnicode_AsUTF8(name);
    if (name_u8) {
        return;
    }
    _PyErr_Format(tstate, PyExc_NameError, format_str, name_u8);

    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    if (PyErr_GivenExceptionMatches(value, PyExc_NameError)) {
        PyObject_SetAttrString(value, "name", name);
    }
    PyErr_Restore(type, value, traceback);
}

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

static Py_hash_t getHash(PyObject *name) {
    auto hash = ((PyASCIIObject *) name)->hash;
    if (hash == -1) {
        hash = PyObject_Hash(name);
    }
    return hash;
}

static PyObject *loadGlobalOrBuiltin(SimplePyFrame *f, PyObject *name, Py_hash_t hash) {
    if (auto v = _PyDict_GetItem_KnownHash(f->globals, name, hash)) {
        Py_INCREF(v);
        return v;
    } else if (_PyErr_Occurred(f->tstate)) {
        return nullptr;
    }

    if (PyDict_CheckExact(f->builtins)) {
        if (auto v = _PyDict_GetItem_KnownHash(f->builtins, name, hash)) {
            Py_INCREF(v);
            return v;
        } else if (!_PyErr_Occurred(f->tstate)) {
            raiseNameError(f->tstate, name);
        }
    } else {
        if (auto v = PyObject_GetItem(f->builtins, name)) {
            return v;
        } else if (_PyErr_ExceptionMatches(f->tstate, PyExc_KeyError)) {
            raiseNameError(f->tstate, name);
        }
    }
    return nullptr;
}

PyObject *handle_LOAD_GLOBAL(SimplePyFrame *f, PyOparg oparg) {
    // NOTE：global似乎默认必须就是dict
    auto name = PyTuple_GET_ITEM(f->code->co_names, oparg);
    assert(PyUnicode_CheckExact(name));
    auto hash = getHash(name);
    if (hash == -1) {
        return nullptr;
    }
    return loadGlobalOrBuiltin(f, name, hash);
}

PyObject *handle_LOAD_NAME(SimplePyFrame *f, PyOparg oparg) {
    auto name = PyTuple_GET_ITEM(f->code->co_names, oparg);
    if (!f->locals) {
        _PyErr_Format(f->tstate, PyExc_SystemError, "no locals when loading %R", name);
        return nullptr;
    }
    auto hash = getHash(name);
    if (hash == -1) {
        return nullptr;
    }

    if (PyDict_CheckExact(f->locals)) {
        if (auto v = _PyDict_GetItem_KnownHash(f->locals, name, hash)) {
            Py_INCREF(v);
            return v;
        } else if (_PyErr_Occurred(f->tstate)) {
            return nullptr;
        }
    } else {
        if (auto v = PyObject_GetItem(f->locals, name)) {
            return v;
        } else if (!_PyErr_ExceptionMatches(f->tstate, PyExc_KeyError)) {
            return nullptr;
        }
        _PyErr_Clear(f->tstate);
    }

    return loadGlobalOrBuiltin(f, name, hash);
}

PyObject *handle_LOAD_ATTR(SimplePyFrame *f, PyOparg oparg, PyObject *owner) {
    auto name = PyTuple_GET_ITEM(f->code->co_names, oparg);
    return PyObject_GetAttr(owner, name);
}

PyObject *handle_LOAD_METHOD(SimplePyFrame *f, PyOparg oparg, PyObject *obj, PyObject **sp) {
    // TODO: 直接生成，省去CALL_METHOD判断, INCREF优化
    auto name = PyTuple_GET_ITEM(f->code->co_names, oparg);
    PyObject *meth = nullptr;
    int meth_found = _PyObject_GetMethod(obj, name, &meth);

    if (!meth) {
        return nullptr;
    }

    if (meth_found) {
        sp[0] = meth;
        Py_INCREF(obj);
        sp[1] = obj;
    } else {
        sp[0] = nullptr;
        sp[1] = meth;
    }
    return meth;
}

int handle_STORE_NAME(SimplePyFrame *f, PyOparg oparg, PyObject *value) {
    auto name = PyTuple_GET_ITEM(f->code->co_names, oparg);
    if (f->locals) {
        int err;
        if (PyDict_CheckExact(f->locals)) {
            err = PyDict_SetItem(f->locals, name, value);
        } else {
            err = PyObject_SetItem(f->locals, name, value);
        }
        return err == 0;
    } else {
        _PyErr_Format(f->tstate, PyExc_SystemError, "no locals found when storing %R", name);
        return 0;
    }
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

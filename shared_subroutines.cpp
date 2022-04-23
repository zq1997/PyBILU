#include "shared_symbols.h"

#include <Python.h>
#include <frameobject.h>
#include <internal/pycore_code.h>
#include <internal/pycore_pyerrors.h>

// TODO: 在想，能不能设计俩版本，decref在这里实现

[[noreturn]] static void gotoErrorHandler() {
    auto tstate = PyThreadState_Get();
    auto cframe = static_cast<ExtendedCFrame &>(*tstate->cframe);
    longjmp(cframe.frame_jmp_buf, 1);
}

static void raiseUndefinedName(PyThreadState *tstate, PyObject *name,
        const char *format_str = "name '%.200s' is not defined") {
    auto name_u8 = PyUnicode_AsUTF8(name);
    if (!name_u8) {
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

static void raiseUndefinedLocal(PyThreadState *tstate, PyObject *name) {
    auto name_u8 = PyUnicode_AsUTF8(name);
    if (!name_u8) {
        return;
    }
    _PyErr_Format(tstate, PyExc_UnboundLocalError,
            "local variable '%.200s' referenced before assignment",
            name_u8);
}

static void raiseUndefinedFree(PyThreadState *tstate, PyObject *name) {
    raiseUndefinedName(tstate, name, "free variable '%.200s' referenced before assignment in enclosing scope");
}

static void raiseUndefinedFreeOrCell(PyFrameObject *f, PyOparg oparg) {
    auto tstate = PyThreadState_Get();
    if (oparg < PyTuple_GET_SIZE(f->f_code->co_cellvars)) {
        raiseUndefinedLocal(tstate, PyTuple_GET_ITEM(f->f_code->co_cellvars, oparg));
    } else {
        auto name = PyTuple_GET_ITEM(f->f_code->co_freevars, oparg - PyTuple_GET_SIZE(f->f_code->co_cellvars));
        raiseUndefinedFree(tstate, name);
    }
}

void handleError_LOAD_FAST(PyFrameObject *f, PyOparg oparg) {
    auto tstate = PyThreadState_Get();
    auto name = PyTuple_GET_ITEM(f->f_code->co_varnames, oparg);
    raiseUndefinedLocal(tstate, name);
    gotoErrorHandler();
}

PyObject *handle_LOAD_CLASSDEREF(PyFrameObject *f, PyOparg oparg) {
    auto tstate = PyThreadState_Get(); // TODO: 延迟获取
    auto locals = f->f_locals;
    assert(locals);
    auto co = f->f_code;
    auto free_index = oparg - PyTuple_GET_SIZE(co->co_cellvars);
    assert(free_index >= 0 && free_index < PyTuple_GET_SIZE(co->co_freevars));
    auto name = PyTuple_GET_ITEM(co->co_freevars, free_index);
    if (PyDict_CheckExact(locals)) {
        auto value = PyDict_GetItemWithError(locals, name);
        if (value) {
            Py_INCREF(value);
            return value;
        } else if (_PyErr_Occurred(tstate)) {
            return nullptr;
        }
    } else {
        auto value = PyObject_GetItem(locals, name);
        if (value) {
            return value;
        } else {
            if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                return nullptr;
            }
            _PyErr_Clear(tstate);
        }
    }
    auto value = PyCell_GET(f->f_localsplus[f->f_code->co_nlocals + oparg]);
    if (value) {
        Py_INCREF(value);
        return value;
    }
    raiseUndefinedFree(tstate, name);
    return nullptr;
}

static Py_hash_t getHash(PyObject *name) {
    auto hash = ((PyASCIIObject *) name)->hash;
    if (hash == -1) {
        hash = PyObject_Hash(name);
    }
    return hash;
}

static PyObject *loadGlobalOrBuiltin(PyFrameObject *f, PyObject *name, Py_hash_t hash) {
    auto tstate = PyThreadState_Get();
    if (auto v = _PyDict_GetItem_KnownHash(f->f_globals, name, hash)) {
        Py_INCREF(v);
        return v;
    } else if (_PyErr_Occurred(tstate)) {
        return nullptr;
    }

    if (PyDict_CheckExact(f->f_builtins)) {
        if (auto v = _PyDict_GetItem_KnownHash(f->f_builtins, name, hash)) {
            Py_INCREF(v);
            return v;
        } else if (!_PyErr_Occurred(tstate)) {
            raiseUndefinedName(tstate, name);
        }
    } else {
        if (auto v = PyObject_GetItem(f->f_builtins, name)) {
            return v;
        } else if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
            raiseUndefinedName(tstate, name);
        }
    }
    return nullptr;
}

PyObject *handle_LOAD_GLOBAL(PyFrameObject *f, PyOparg oparg) {
    // NOTE：global似乎默认必须就是dict
    auto name = PyTuple_GET_ITEM(f->f_code->co_names, oparg);
    assert(PyUnicode_CheckExact(name));
    auto hash = getHash(name);
    if (hash == -1) {
        return nullptr;
    }
    return loadGlobalOrBuiltin(f, name, hash);
}

PyObject *handle_LOAD_NAME(PyFrameObject *f, PyOparg oparg) {
    auto tstate = PyThreadState_Get();
    auto name = PyTuple_GET_ITEM(f->f_code->co_names, oparg);
    if (!f->f_locals) {
        _PyErr_Format(tstate, PyExc_SystemError, "no locals when loading %R", name);
        return nullptr;
    }
    auto hash = getHash(name);
    if (hash == -1) {
        return nullptr;
    }

    if (PyDict_CheckExact(f->f_locals)) {
        if (auto v = _PyDict_GetItem_KnownHash(f->f_locals, name, hash)) {
            Py_INCREF(v);
            return v;
        } else if (_PyErr_Occurred(tstate)) {
            return nullptr;
        }
    } else {
        if (auto v = PyObject_GetItem(f->f_locals, name)) {
            return v;
        } else if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
            return nullptr;
        }
        _PyErr_Clear(tstate);
    }

    return loadGlobalOrBuiltin(f, name, hash);
}

PyObject *handle_LOAD_ATTR(PyFrameObject *f, PyOparg oparg, PyObject *owner) {
    auto name = PyTuple_GET_ITEM(f->f_code->co_names, oparg);
    return PyObject_GetAttr(owner, name);
}

PyObject *handle_LOAD_METHOD(PyFrameObject *f, PyOparg oparg, PyObject *obj, PyObject **sp) {
    // TODO: 直接生成，省去CALL_METHOD判断, INCREF优化
    auto name = PyTuple_GET_ITEM(f->f_code->co_names, oparg);
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

int handle_STORE_NAME(PyFrameObject *f, PyOparg oparg, PyObject *value) {
    auto name = PyTuple_GET_ITEM(f->f_code->co_names, oparg);
    if (f->f_locals) {
        int err;
        if (PyDict_CheckExact(f->f_locals)) {
            err = PyDict_SetItem(f->f_locals, name, value);
        } else {
            err = PyObject_SetItem(f->f_locals, name, value);
        }
        return err == 0;
    } else {
        auto tstate = PyThreadState_Get();
        _PyErr_Format(tstate, PyExc_SystemError, "no locals found when storing %R", name);
        return 0;
    }
}

int handle_DELETE_DEREF(PyFrameObject *f, PyOparg oparg) {
    PyObject *cell = f->f_localsplus[f->f_code->co_nlocals + oparg];
    PyObject *oldobj = PyCell_GET(cell);
    if (oldobj) {
        PyCell_SET(cell, nullptr);
        Py_DECREF(oldobj);
        return 1;
    }
    raiseUndefinedFreeOrCell(f, oparg);
    return 0;
}

int handle_DELETE_GLOBAL(PyFrameObject *f, PyOparg oparg) {
    auto name = PyTuple_GET_ITEM(f->f_code->co_names, oparg);
    int err = PyDict_DelItem(f->f_globals, name);
    if (err != 0) {
        auto tstate = PyThreadState_Get();
        if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
            raiseUndefinedName(tstate, name);
        }
        return 0;
    }
    return 1;
}

int handle_DELETE_NAME(PyFrameObject *f, PyOparg oparg) {
    auto name = PyTuple_GET_ITEM(f->f_code->co_names, oparg);
    PyObject *ns = f->f_locals;
    auto tstate = PyThreadState_Get();
    if (ns) {
        if (PyObject_DelItem(ns, name) != 0) {
            raiseUndefinedName(tstate, name);
            return 0;
        }
        return 1;
    }
    _PyErr_Format(tstate, PyExc_SystemError, "no locals when deleting %R", name);
    return 0;
}

PyObject *handle_UNARY_NOT(PyObject *value) {
    auto res = PyObject_IsTrue(value);
    if (res < 0) [[unlikely]] {
        gotoErrorHandler();
    }
    static const std::array bool_values{Py_False, Py_True};
    auto not_value = bool_values[res == 0];
    Py_INCREF(not_value);
    return not_value;
}

PyObject *handle_BINARY_POWER(PyObject *base, PyObject *exp) {
    auto res = PyNumber_Power(base, exp, Py_None);
    if (!res) [[unlikely]] {
        gotoErrorHandler();
    }
    return res;
}

PyObject *handle_INPLACE_POWER(PyObject *base, PyObject *exp) {
    return PyNumber_InPlacePower(base, exp, Py_None);
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

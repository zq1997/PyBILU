#include "shared_symbols.h"

#include <Python.h>
#include <opcode.h>
#include <frameobject.h>
#include <internal/pycore_pystate.h>
#include <internal/pycore_code.h>
#include <internal/pycore_pyerrors.h>
#include <internal/pycore_abstract.h>
#include <iostream>

using namespace std;

static PyObject *const python_bool_values[]{Py_False, Py_True};

void handle_DECREF(PyObject *obj) {
    if (!--obj->ob_refcnt) {
        Py_TYPE(obj)->tp_dealloc(obj);
    }
}

void handle_XDECREF(PyObject *obj) {
    if (obj && !--obj->ob_refcnt) {
        Py_TYPE(obj)->tp_dealloc(obj);
    }
}

// TODO: 在想，能不能设计俩版本，decref在这里实现
// TODO: 很多函数是否应该展开，把Python的实现复制过来，降低调用层数
// TODO: 能否设置hot inline等确保展开

[[noreturn]] static void gotoErrorHandler(PyThreadState *tstate) {
    auto cframe = static_cast<ExtendedCFrame &>(*tstate->cframe);
    longjmp(cframe.frame_jmp_buf, 1);
}

[[noreturn]] static void gotoErrorHandler() {
    gotoErrorHandler(_PyThreadState_GET());
}

template <typename... T>
static inline void gotoErrorHandler(bool cond, T... args) {
    // TODO: 是不是都是unlikely
    if (cond) [[unlikely]] {
        gotoErrorHandler(std::forward<T>(args)...);
    }
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

void raiseException() {
    auto tstate = _PyThreadState_GET();
    auto frame = tstate->frame;
    auto code = frame->f_code;
    auto instr_array = reinterpret_cast<PyInstr *>(&PyTuple_GET_ITEM(code->co_code, 0));
    auto i = frame->f_lasti;
    auto opcode = _Py_OPCODE(instr_array[i]);
    auto oparg = _Py_OPARG(instr_array[i]);
    unsigned shift = 0;
    while (i && _Py_OPCODE(instr_array[--i]) == EXTENDED_ARG) {
        shift += EXTENDED_ARG_BITS;
        oparg |= _Py_OPARG(instr_array[i]) << shift;
    }
    assert(opcode == LOAD_FAST);
    if (opcode == LOAD_FAST || opcode == DELETE_FAST) {
        auto name = PyTuple_GET_ITEM(code->co_varnames, oparg);
        raiseUndefinedLocal(tstate, name);
    } else if (opcode == LOAD_DEREF || opcode == DELETE_DEREF) {
        auto cell_num = PyTuple_GET_SIZE(code->co_cellvars);
        if (oparg < cell_num) {
            raiseUndefinedLocal(tstate, PyTuple_GET_ITEM(code->co_cellvars, oparg));
        } else {
            auto name = PyTuple_GET_ITEM(code->co_freevars, oparg - cell_num);
            raiseUndefinedFree(tstate, name);
        }
    } else {
        _PyErr_Format(tstate, PyExc_SystemError, "error block遇到了未知opcode");
    }
    gotoErrorHandler(tstate);
}

PyObject *handle_LOAD_CLASSDEREF(PyFrameObject *f, PyOparg oparg) {
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
        } else {
            auto tstate = _PyThreadState_GET();
            gotoErrorHandler(_PyErr_Occurred(tstate), tstate);
        }
    } else {
        auto value = PyObject_GetItem(locals, name);
        if (value) {
            return value;
        } else {
            auto tstate = _PyThreadState_GET();
            gotoErrorHandler(!_PyErr_ExceptionMatches(tstate, PyExc_KeyError), tstate);
            _PyErr_Clear(tstate);
        }
    }
    auto value = PyCell_GET(f->f_localsplus[f->f_code->co_nlocals + oparg]);
    if (value) {
        Py_INCREF(value);
        return value;
    }
    auto tstate = _PyThreadState_GET();
    raiseUndefinedFree(tstate, name);
    gotoErrorHandler(tstate);
}

static Py_hash_t getHash(PyObject *name) {
    assert(PyUnicode_CheckExact(name));
    auto hash = ((PyASCIIObject *) name)->hash;
    if (hash == -1) {
        hash = PyObject_Hash(name);
    }
    return hash;
}

static PyObject *loadGlobalOrBuiltin(PyFrameObject *f, PyObject *name, Py_hash_t hash) {
    if (auto v = _PyDict_GetItem_KnownHash(f->f_globals, name, hash)) {
        Py_INCREF(v);
        return v;
    } else {
        auto tstate = _PyThreadState_GET();
        gotoErrorHandler(_PyErr_Occurred(tstate), tstate);
    }

    if (PyDict_CheckExact(f->f_builtins)) {
        if (auto v = _PyDict_GetItem_KnownHash(f->f_builtins, name, hash)) {
            Py_INCREF(v);
            return v;
        } else {
            auto tstate = _PyThreadState_GET();
            if (!_PyErr_Occurred(tstate)) {
                raiseUndefinedName(tstate, name);
            }
            gotoErrorHandler(tstate);
        }
    } else {
        if (auto v = PyObject_GetItem(f->f_builtins, name)) {
            return v;
        } else {
            auto tstate = _PyThreadState_GET();
            if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                raiseUndefinedName(tstate, name);
            }
            gotoErrorHandler(tstate);
        }
    }
}

PyObject *handle_LOAD_GLOBAL(PyFrameObject *f, PyObject *name) {
    auto hash = getHash(name);
    gotoErrorHandler(hash == -1);
    return loadGlobalOrBuiltin(f, name, hash);
}

void handle_STORE_GLOBAL(PyFrameObject *f, PyObject *name, PyObject *value) {
    auto err = PyDict_SetItem(f->f_globals, name, value);
    gotoErrorHandler(err);
}

void handle_DELETE_GLOBAL(PyFrameObject *f, PyObject *name) {
    int err = PyDict_DelItem(f->f_globals, name);
    // TODO: 类似地，unlikely都加上
    if (err) [[unlikely]] {
        auto tstate = _PyThreadState_GET();
        if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
            raiseUndefinedName(tstate, name);
        }
        gotoErrorHandler(tstate);
    }
}

PyObject *handle_LOAD_NAME(PyFrameObject *f, PyObject *name) {
    if (!f->f_locals) {
        auto tstate = _PyThreadState_GET();
        _PyErr_Format(tstate, PyExc_SystemError, "no locals when loading %R", name);
        gotoErrorHandler(tstate);
    }
    auto hash = getHash(name);
    gotoErrorHandler(hash == -1);

    if (PyDict_CheckExact(f->f_locals)) {
        if (auto v = _PyDict_GetItem_KnownHash(f->f_locals, name, hash)) {
            Py_INCREF(v);
            return v;
        } else {
            auto tstate = _PyThreadState_GET();
            gotoErrorHandler(_PyErr_Occurred(tstate), tstate);
        }
    } else {
        if (auto v = PyObject_GetItem(f->f_locals, name)) {
            return v;
        } else {
            auto tstate = _PyThreadState_GET();
            gotoErrorHandler(!_PyErr_ExceptionMatches(tstate, PyExc_KeyError), tstate);
            _PyErr_Clear(tstate);
        }
    }

    return loadGlobalOrBuiltin(f, name, hash);
}

void handle_STORE_NAME(PyFrameObject *f, PyObject *name, PyObject *value) {
    if (!f->f_locals) {
        auto tstate = _PyThreadState_GET();
        _PyErr_Format(tstate, PyExc_SystemError, "no locals found when storing %R", name);
        gotoErrorHandler(tstate);
    }
    int err;
    if (PyDict_CheckExact(f->f_locals)) {
        err = PyDict_SetItem(f->f_locals, name, value);
    } else {
        err = PyObject_SetItem(f->f_locals, name, value);
    }
    gotoErrorHandler(err);
}

void handle_DELETE_NAME(PyFrameObject *f, PyObject *name) {
    // TODO: 类似这种东西，感觉可以放到主程序中直接执行，失败则进行二次校验
    PyObject *ns = f->f_locals;
    if (!ns) [[unlikely]] {
        auto tstate = _PyThreadState_GET();
        _PyErr_Format(tstate, PyExc_SystemError, "no locals when deleting %R", name);
        gotoErrorHandler(tstate);
    }
    auto err = PyObject_DelItem(ns, name);
    if (err) [[unlikely]] {
        auto tstate = _PyThreadState_GET();
        raiseUndefinedName(tstate, name);
        gotoErrorHandler(tstate);
    }
}

PyObject *handle_LOAD_ATTR(PyObject *owner, PyObject *name) {
    auto value = PyObject_GetAttr(owner, name);
    gotoErrorHandler(!value);
    return value;
}

PyObject *handle_LOAD_METHOD(PyObject *obj, PyObject *name, PyObject **sp) {
    PyObject *meth = nullptr;
    int meth_found = _PyObject_GetMethod(obj, name, &meth);
    gotoErrorHandler(!meth);

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

void handle_STORE_ATTR(PyObject *owner, PyObject *name, PyObject *value) {
    // TODO: 把它展开
    auto err = PyObject_SetAttr(owner, name, value);
    gotoErrorHandler(err);
}

PyObject *handle_BINARY_SUBSCR(PyObject *container, PyObject *sub) {
    auto value = PyObject_GetItem(container, sub);
    gotoErrorHandler(!value);
    return value;
}

void handle_STORE_SUBSCR(PyObject *container, PyObject *sub, PyObject *value) {
    auto err = PyObject_SetItem(container, sub, value);
    gotoErrorHandler(err);
}

static const char *getSlotSign(size_t offset) {
    switch (offset) {
    case offsetof(PyNumberMethods, nb_add):
        return "+";
    case offsetof(PyNumberMethods, nb_inplace_add):
        return "+=";
    case offsetof(PyNumberMethods, nb_subtract):
        return "-";
    case offsetof(PyNumberMethods, nb_inplace_subtract):
        return "-=";
    case offsetof(PyNumberMethods, nb_multiply):
        return "*";
    case offsetof(PyNumberMethods, nb_inplace_multiply):
        return "*=";
    case offsetof(PyNumberMethods, nb_floor_divide):
        return "//";
    case offsetof(PyNumberMethods, nb_inplace_floor_divide):
        return "//=";
    case offsetof(PyNumberMethods, nb_true_divide):
        return "/";
    case offsetof(PyNumberMethods, nb_inplace_true_divide):
        return "/=";
    case offsetof(PyNumberMethods, nb_remainder):
        return "%";
    case offsetof(PyNumberMethods, nb_inplace_remainder):
        return "%=";
    case offsetof(PyNumberMethods, nb_power):
        return "**";
    case offsetof(PyNumberMethods, nb_inplace_power):
        return "**=";
    case offsetof(PyNumberMethods, nb_matrix_multiply):
        return "@";
    case offsetof(PyNumberMethods, nb_inplace_matrix_multiply):
        return "@=";
    case offsetof(PyNumberMethods, nb_lshift):
        return "<<";
    case offsetof(PyNumberMethods, nb_inplace_lshift):
        return "<<=";
    case offsetof(PyNumberMethods, nb_rshift):
        return ">>";
    case offsetof(PyNumberMethods, nb_inplace_rshift):
        return ">>=";
    case offsetof(PyNumberMethods, nb_and):
        return "&";
    case offsetof(PyNumberMethods, nb_inplace_and):
        return "&=";
    case offsetof(PyNumberMethods, nb_or):
        return "|";
    case offsetof(PyNumberMethods, nb_inplace_or):
        return "|=";
    case offsetof(PyNumberMethods, nb_xor):
        return "^";
    case offsetof(PyNumberMethods, nb_inplace_xor):
        return "^=";
    default:
        assert(false);
        return nullptr;
    }
}

template <typename T>
static const char *getSlotSign(T PyNumberMethods::* op_slot) {
    PyNumberMethods dummy;
    auto offset = reinterpret_cast<char *>(&(dummy.*op_slot)) - reinterpret_cast<char *>(&dummy);
    return getSlotSign(offset);
}

static void checkSlotResult(PyObject *obj, const char *slot_name, PyObject *result) {
#ifndef NDEBUG
    PyThreadState *tstate = _PyThreadState_GET();
    if (result) {
        if (_PyErr_Occurred(tstate)) {
            _Py_FatalErrorFormat(__func__,
                    "Slot %s of type %s succeeded with an exception set",
                    slot_name, Py_TYPE(obj)->tp_name);
        }
    } else {
        if (!_PyErr_Occurred(tstate)) {
            _Py_FatalErrorFormat(__func__,
                    "Slot %s of type %s failed without setting an exception",
                    slot_name, Py_TYPE(obj)->tp_name);
        }
    }
#endif
}

template <typename T>
static void checkSlotResult(PyObject *obj, T PyNumberMethods::* op_slot, PyObject *result) {
    checkSlotResult(obj, getSlotSign(op_slot), result);
}

PyObject *handle_UNARY_NOT(PyObject *value) {
    auto res = PyObject_IsTrue(value);
    if (res < 0) [[unlikely]] {
        gotoErrorHandler();
    }
    auto not_value = python_bool_values[res == 0];
    Py_INCREF(not_value);
    return not_value;
}

template <unaryfunc PyNumberMethods::*P, char op_sign, const char *slot_name>
PyObject *handle_UNARY(PyObject *value) {
    auto type = Py_TYPE(value);
    auto *m = type->tp_as_number;
    if (m && m->*P) [[likely]] {
        PyObject *res = (m->*P)(value);
        checkSlotResult(value, slot_name, res);
        return res;
    }
    PyErr_Format(PyExc_TypeError, "bad operand type for unary %c: '%.200s'", op_sign, type->tp_name);
    gotoErrorHandler();
}

PyObject *handle_UNARY_POSITIVE(PyObject *value) {
    static constexpr char slot[]{"__pos__"};
    return handle_UNARY<&PyNumberMethods::nb_positive, '+', slot>(value);
}

PyObject *handle_UNARY_NEGATIVE(PyObject *value) {
    static constexpr char slot[]{"__neg__"};
    return handle_UNARY<&PyNumberMethods::nb_negative, '-', slot>(value);
}

PyObject *handle_UNARY_INVERT(PyObject *value) {
    static constexpr char slot[]{"__invert__"};
    return handle_UNARY<&PyNumberMethods::nb_invert, '~', slot>(value);
}

template <typename T>
[[noreturn]] static void raiseBinOpTypeError(PyObject *v, PyObject *w, T op_slot, const char *hint = "") {
    PyErr_Format(PyExc_TypeError,
            "unsupported operand type(s) for %.100s: '%.100s' and '%.100s'%s",
            getSlotSign(op_slot),
            Py_TYPE(v)->tp_name,
            Py_TYPE(w)->tp_name,
            hint);
    gotoErrorHandler();
}

template <bool is_ternary = false, typename T, typename U>
static auto callSlotOfBinOp(PyObject *v, PyObject *w, T slot, U err_msg_slot) {
    PyObject *result;
    if constexpr (is_ternary) {
        result = slot(v, w, Py_None);
    } else {
        result = slot(v, w);
    }
    checkSlotResult(v, err_msg_slot, result);
    gotoErrorHandler(!result);
    return result;
}

template <bool force_return = false, typename T, typename... Ts>
static PyObject *handleBinary(PyObject *v, PyObject *w, T op_slot, Ts... more_op_slots) {
    constexpr auto is_ternary = is_same_v<T, ternaryfunc PyNumberMethods::*>;
    auto type_v = Py_TYPE(v);
    auto type_w = Py_TYPE(w);
    auto slots_v = type_v->tp_as_number;
    auto slots_w = type_w->tp_as_number;
    auto err_msg_slot = op_slot;

    if constexpr(sizeof...(Ts)) {
        static_assert(sizeof...(Ts) == 1);
        if (auto slot = slots_v ? slots_v->*op_slot : nullptr) {
            auto result = callSlotOfBinOp<is_ternary>(v, w, slot, err_msg_slot);
            if (result != Py_NotImplemented) {
                return result;
            }
            Py_DECREF(result);
        }
        op_slot = std::get<0>(std::tuple{more_op_slots...});
    }

    auto slot_v = slots_v ? slots_v->*op_slot : nullptr;
    auto slot_w = slots_w ? slots_w->*op_slot : nullptr;
    slot_w = slot_w == slot_v ? nullptr : slot_w;

    if (slot_v) {
        if (slot_w && PyType_IsSubtype(type_w, type_v)) {
            auto result = callSlotOfBinOp<is_ternary>(v, w, slot_w, err_msg_slot);
            if (result != Py_NotImplemented) {
                return result;
            }
            Py_DECREF(result);
            slot_w = nullptr;
        }
        auto result = callSlotOfBinOp<is_ternary>(v, w, slot_v, err_msg_slot);
        if (result != Py_NotImplemented) {
            return result;
        }
        Py_DECREF(result);
    }
    if (slot_w) {
        auto result = callSlotOfBinOp<is_ternary>(v, w, slot_w, err_msg_slot);
        if (result != Py_NotImplemented) {
            return result;
        }
        Py_DECREF(result);
    }
    if constexpr (force_return) {
        return nullptr;
    } else {
        raiseBinOpTypeError(v, w, err_msg_slot);
    }
}

PyObject *handle_BINARY_ADD(PyObject *v, PyObject *w) {
    constexpr auto op_slot = &PyNumberMethods::nb_add;
    auto result = handleBinary<true>(v, w, op_slot);
    if (result) {
        return result;
    }
    auto m = Py_TYPE(v)->tp_as_sequence;
    if (!m || !m->sq_concat) {
        result = (m->sq_concat)(v, w);
        checkSlotResult(v, op_slot, result);
        gotoErrorHandler(!result);
        return result;
    }
    raiseBinOpTypeError(v, w, op_slot);
}

PyObject *handle_INPLACE_ADD(PyObject *v, PyObject *w) {
    constexpr auto iop_slot = &PyNumberMethods::nb_inplace_add;
    auto result = handleBinary<true>(v, w, iop_slot, &PyNumberMethods::nb_add);
    if (result) {
        return result;
    }
    auto m = Py_TYPE(v)->tp_as_sequence;
    if (m) {
        auto func = m->sq_inplace_concat ? m->sq_inplace_concat : m->sq_concat;
        if (func) {
            result = func(v, w);
            checkSlotResult(v, iop_slot, result);
            gotoErrorHandler(!result);
            return result;
        }
    }
    raiseBinOpTypeError(v, w, iop_slot);
}

PyObject *handle_BINARY_SUBTRACT(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_subtract);
}

PyObject *handle_INPLACE_SUBTRACT(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_subtract, &PyNumberMethods::nb_subtract);
}

static PyObject *repeatSequence(PyObject *v, PyObject *w, binaryfunc PyNumberMethods::*op_slot) {
    ssizeargfunc repeat_func = nullptr;
    PyObject *seq;
    PyObject *n;
    auto mv = Py_TYPE(v)->tp_as_sequence;
    auto mw = Py_TYPE(w)->tp_as_sequence;
    if (mv) {
        if (op_slot == &PyNumberMethods::nb_inplace_multiply) {
            repeat_func = mv->sq_inplace_repeat ? mv->sq_inplace_repeat : mv->sq_repeat;
        } else {
            repeat_func = mv->sq_repeat;
        }
        seq = v;
        n = w;
    }
    if (!repeat_func) {
        if (mw && mw->sq_repeat) {
            repeat_func = mw->sq_repeat;
            seq = w;
            n = v;
        } else {
            raiseBinOpTypeError(v, w, op_slot);
        }
    }

    if (!_PyIndex_Check(n)) {
        PyErr_Format(PyExc_TypeError, "can't multiply sequence by non-int of type '%.200s'", Py_TYPE(n)->tp_name);
        gotoErrorHandler();
    }
    auto count = PyNumber_AsSsize_t(n, PyExc_OverflowError);
    if (count == -1) {
        auto tstate = _PyThreadState_GET();
        if (_PyErr_Occurred(tstate)) {
            gotoErrorHandler(tstate);
        }
    }
    auto result = repeat_func(seq, count);
    checkSlotResult(seq, op_slot, result);
    gotoErrorHandler(!result);
    return result;
}

PyObject *handle_BINARY_MULTIPLY(PyObject *v, PyObject *w) {
    constexpr auto op_slot = &PyNumberMethods::nb_multiply;
    auto result = handleBinary<true>(v, w, op_slot);
    if (result) {
        return result;
    }
    return repeatSequence(v, w, op_slot);
}


PyObject *handle_INPLACE_MULTIPLY(PyObject *v, PyObject *w) {
    constexpr auto iop_slot = &PyNumberMethods::nb_inplace_multiply;
    auto result = handleBinary<true>(v, w, iop_slot, &PyNumberMethods::nb_multiply);
    if (result) {
        return result;
    }
    return repeatSequence(v, w, iop_slot);
}

PyObject *handle_BINARY_FLOOR_DIVIDE(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_floor_divide);
}

PyObject *handle_INPLACE_FLOOR_DIVIDE(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_floor_divide, &PyNumberMethods::nb_floor_divide);
}

PyObject *handle_BINARY_TRUE_DIVIDE(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_true_divide);
}

PyObject *handle_INPLACE_TRUE_DIVIDE(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_true_divide, &PyNumberMethods::nb_true_divide);
}

PyObject *handle_BINARY_MODULO(PyObject *v, PyObject *w) {
    if (PyUnicode_CheckExact(v) && (PyUnicode_CheckExact(w) || !PyUnicode_Check(w))) {
        // fast path
        auto res = PyUnicode_Format(v, w);
        gotoErrorHandler(!res);
        return res;
    } else {
        return handleBinary(v, w, &PyNumberMethods::nb_remainder);
    }
}

PyObject *handle_INPLACE_MODULO(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_remainder, &PyNumberMethods::nb_remainder);
}

PyObject *handle_BINARY_POWER(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_power);
}

PyObject *handle_INPLACE_POWER(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_power, &PyNumberMethods::nb_power);
}

PyObject *handle_BINARY_MATRIX_MULTIPLY(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_matrix_multiply);
}

PyObject *handle_INPLACE_MATRIX_MULTIPLY(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_matrix_multiply, &PyNumberMethods::nb_matrix_multiply);
}

PyObject *handle_BINARY_LSHIFT(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_lshift);
}

PyObject *handle_INPLACE_LSHIFT(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_lshift, &PyNumberMethods::nb_lshift);
}

PyObject *handle_BINARY_RSHIFT(PyObject *v, PyObject *w) {
    constexpr auto op_slot = &PyNumberMethods::nb_rshift;
    auto result = handleBinary<true>(v, w, op_slot);
    if (result) {
        return result;
    }
    auto hint = PyCFunction_CheckExact(v)
            && !strcmp("print", reinterpret_cast<PyCFunctionObject *>(v)->m_ml->ml_name) ?
            " Did you mean \"print(<message>, file=<output_stream>)\"?" : "";
    raiseBinOpTypeError(v, w, op_slot, hint);
}

PyObject *handle_INPLACE_RSHIFT(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_rshift, &PyNumberMethods::nb_rshift);
}

PyObject *handle_BINARY_AND(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_and);
}

PyObject *handle_INPLACE_AND(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_and, &PyNumberMethods::nb_and);
}

PyObject *handle_BINARY_OR(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_or);
}

PyObject *handle_INPLACE_OR(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_or, &PyNumberMethods::nb_or);
}

PyObject *handle_BINARY_XOR(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_xor);
}

PyObject *handle_INPLACE_XOR(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_xor, &PyNumberMethods::nb_xor);
}

PyObject *handle_COMPARE_OP(PyObject *v, PyObject *w, int op) {
    auto type_v = Py_TYPE(v);
    auto type_w = Py_TYPE(w);
    auto slot_v = type_v->tp_richcompare;
    auto slot_w = type_w->tp_richcompare;

    // TODO: gotoErrorHandler和Py_NotImplemented先后考虑考虑
    if (slot_w && type_v != type_w && PyType_IsSubtype(type_w, type_v)) {
        auto res = slot_w(w, v, _Py_SwappedOp[op]);
        gotoErrorHandler(!res);
        if (res != Py_NotImplemented) {
            return res;
        }
        Py_DECREF(res);
        slot_w = nullptr;
    }
    if (slot_v) {
        auto res = slot_v(v, w, op);
        gotoErrorHandler(!res);
        if (res != Py_NotImplemented) {
            return res;
        }
        Py_DECREF(res);
    }
    if (slot_w) {
        auto res = slot_w(w, v, _Py_SwappedOp[op]);
        gotoErrorHandler(!res);
        if (res != Py_NotImplemented) {
            return res;
        }
        Py_DECREF(res);
    }
    if (op == Py_EQ || op == Py_NE) {
        auto res = python_bool_values[(v == w) ^ (op == Py_NE)];
        Py_INCREF(res);
        return res;
    }

    static const char *const op_signs[] = {"<", "<=", "==", "!=", ">", ">="};
    auto tstate = _PyThreadState_GET();
    // TODO: 异常抛出给统一化
    _PyErr_Format(tstate, PyExc_TypeError,
            "'%s' not supported between instances of '%.100s' and '%.100s'",
            op_signs[op],
            type_v->tp_name,
            type_w->tp_name);
    gotoErrorHandler(tstate);
}

const auto symbol_names{apply(
        [](auto &&... x) noexcept { return array{x.second ...}; },
        external_symbols
)};
const auto symbol_addresses{apply(
        [](auto &&... x) noexcept { return array{reinterpret_cast<void *>(x.first) ...}; },
        external_symbols
)};

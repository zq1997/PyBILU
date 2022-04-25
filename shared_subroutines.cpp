#include "shared_symbols.h"

#include <Python.h>
#include <opcode.h>
#include <frameobject.h>
#include <internal/pycore_pystate.h>
#include <internal/pycore_code.h>
#include <internal/pycore_pyerrors.h>
#include <internal/pycore_abstract.h>

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

int _Py_CheckSlotResult(PyObject *obj, const char *slot_name, int success) {
    PyThreadState *tstate = _PyThreadState_GET();
    if (!success) {
        if (!_PyErr_Occurred(tstate)) {
            _Py_FatalErrorFormat(__func__,
                    "Slot %s of type %s failed without setting an exception",
                    slot_name, Py_TYPE(obj)->tp_name);
        }
    } else {
        if (_PyErr_Occurred(tstate)) {
            _Py_FatalErrorFormat(__func__,
                    "Slot %s of type %s succeeded with an exception set",
                    slot_name, Py_TYPE(obj)->tp_name);
        }
    }
    return 1;
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

// TODO: 删了
// static PyObject *null_error() {
//     auto tstate = _PyThreadState_GET();
//     // TODO: 需要么，应该是一定没有err叭，tstate我觉得也不必校验
//     if (!_PyErr_Occurred(tstate)) {
//         _PyErr_SetString(tstate, PyExc_SystemError, "null argument to internal routine");
//     }
//     return nullptr;
// }

// static PyObject *type_error(const char *msg, PyObject *obj) {
//     PyErr_Format(PyExc_TypeError, msg, Py_TYPE(obj)->tp_name);
//     return nullptr;
// }

template <unaryfunc PyNumberMethods::*P, char SIGN, const char *SLOT>
PyObject *handle_UNARY(PyObject *value) {
    auto type = Py_TYPE(value);
    auto *m = type->tp_as_number;
    if (m && m->*P) [[unlikely]] {
        PyObject *res = (*(m->*P))(value);
        assert(_Py_CheckSlotResult(value, SLOT, res != nullptr));
        return res;
    }
    PyErr_Format(PyExc_TypeError, "bad operand type for unary %c: '%.200s'", SIGN, type->tp_name);
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

static PyObject *binary_op_impl(PyObject *v, PyObject *w, binaryfunc PyNumberMethods::* op_slot, const char *op_name) {
    auto typev = Py_TYPE(v);
    auto typew = Py_TYPE(w);
    auto slotv = typev->tp_as_number ? typev->tp_as_number->*op_slot : nullptr;
    auto slotw = typew->tp_as_number ? typew->tp_as_number->*op_slot : nullptr;
    slotw = slotw == slotv ? nullptr : slotw;

    if (slotv) {
        PyObject *x;
        if (slotw && PyType_IsSubtype(typew, typev)) {
            x = slotw(v, w);
            assert(_Py_CheckSlotResult(v, op_name, x != nullptr));
            if (x != Py_NotImplemented) {
                gotoErrorHandler(!x);
                return x;
            }
            Py_DECREF(x);
            slotw = nullptr;
        }
        x = slotv(v, w);
        assert(_Py_CheckSlotResult(v, op_name, x != nullptr));
        if (x != Py_NotImplemented) {
            gotoErrorHandler(!x);
            return x;
        }
        Py_DECREF(x);
    }
    if (slotw) {
        PyObject *x = slotw(v, w);
        assert(_Py_CheckSlotResult(w, op_name, x != nullptr));
        if (x != Py_NotImplemented) {
            gotoErrorHandler(!x);
            return x;
        }
        Py_DECREF(x);
    }
    return Py_NotImplemented; // borrowed reference
}

static PyObject *
binary_iop_impl(PyObject *v, PyObject *w, binaryfunc PyNumberMethods::* iop_slot, binaryfunc PyNumberMethods::* op_slot,
        const char *op_name) {
    auto mv = Py_TYPE(v)->tp_as_number;
    if (mv != nullptr) {
        if (auto slot = mv->*iop_slot) {
            PyObject *x = (slot)(v, w);
            assert(_Py_CheckSlotResult(v, op_name, x != nullptr));
            if (x != Py_NotImplemented) {
                gotoErrorHandler(!x);
                return x;
            }
            Py_DECREF(x);
        }
    }
    return binary_op_impl(v, w, op_slot, op_name);
}

[[noreturn]] static void binop_type_error(PyObject *v, PyObject *w, const char *op_name) {
    PyErr_Format(PyExc_TypeError,
            "unsupported operand type(s) for %.100s: '%.100s' and '%.100s'",
            op_name,
            Py_TYPE(v)->tp_name,
            Py_TYPE(w)->tp_name);
    gotoErrorHandler();
}

template <binaryfunc PyNumberMethods::*P, const char *SIGN, const char *NAME = SIGN>
PyObject *binary_op(PyObject *v, PyObject *w) {
    auto result = binary_op_impl(v, w, P, NAME);
    if (result != Py_NotImplemented) {
        return result;
    }

    if constexpr(P == &PyNumberMethods::nb_rshift) {
        if (PyCFunction_CheckExact(v) &&
                strcmp(((PyCFunctionObject *) v)->m_ml->ml_name, "print") == 0) {
            PyErr_Format(PyExc_TypeError,
                    "unsupported operand type(s) for %.100s: "
                    "'%.100s' and '%.100s'. Did you mean \"print(<message>, "
                    "file=<output_stream>)\"?",
                    SIGN,
                    Py_TYPE(v)->tp_name,
                    Py_TYPE(w)->tp_name);
            gotoErrorHandler();
        }
    }
    binop_type_error(v, w, SIGN);
}

template <binaryfunc PyNumberMethods::*IP, binaryfunc PyNumberMethods::*P, const char *SIGN, const char *NAME = SIGN>
PyObject *binary_iop(PyObject *v, PyObject *w) {
    PyObject *result = binary_iop_impl(v, w, IP, P, NAME);
    if (result != Py_NotImplemented) {
        return result;
    }
    binop_type_error(v, w, SIGN);
}

static PyObject *
sequence_repeat(ssizeargfunc repeatfunc, PyObject *seq, PyObject *n) {
    Py_ssize_t count;
    if (_PyIndex_Check(n)) {
        count = PyNumber_AsSsize_t(n, PyExc_OverflowError);
        if (count == -1 && PyErr_Occurred()) {
            gotoErrorHandler();
        }
    } else {
        PyErr_Format(PyExc_TypeError, "can't multiply sequence by non-int of type '%.200s'", Py_TYPE(n)->tp_name);
        gotoErrorHandler();
    }
    PyObject *res = (*repeatfunc)(seq, count);
    assert(_Py_CheckSlotResult(seq, "*", res != nullptr));
    gotoErrorHandler(!res);
    return res;
}

PyObject *handle_BINARY_ADD(PyObject *v, PyObject *w) {
    auto result = binary_op_impl(v, w, &PyNumberMethods::nb_add, "+");
    if (result != Py_NotImplemented) {
        return result;
    }

    auto m = Py_TYPE(v)->tp_as_sequence;
    if (m && m->sq_concat) {
        result = (*m->sq_concat)(v, w);
        assert(_Py_CheckSlotResult(v, "+", result != nullptr));
        gotoErrorHandler(!result);
        return result;
    }

    binop_type_error(v, w, "+");
}

PyObject *handle_INPLACE_ADD(PyObject *v, PyObject *w) {
    auto result = binary_iop_impl(v, w, &PyNumberMethods::nb_inplace_add, &PyNumberMethods::nb_add, "+=");
    if (result != Py_NotImplemented) {
        return result;
    }
    auto m = Py_TYPE(v)->tp_as_sequence;
    if (m) {
        binaryfunc func = m->sq_inplace_concat ? m->sq_inplace_concat : m->sq_concat;
        if (func) {
            result = func(v, w);
            assert(_Py_CheckSlotResult(v, "+=", result != nullptr));
            gotoErrorHandler(!result);
            return result;
        }
    }
    binop_type_error(v, w, "+=");
}

PyObject *handle_BINARY_SUBTRACT(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"-"};
    return binary_op<&PyNumberMethods::nb_subtract, op_sign>(v, w);
}

PyObject *handle_INPLACE_SUBTRACT(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"-="};
    return binary_iop<&PyNumberMethods::nb_inplace_subtract, &PyNumberMethods::nb_subtract, op_sign>(v, w);
}

PyObject *handle_BINARY_MULTIPLY(PyObject *v, PyObject *w) {
    auto result = binary_op_impl(v, w, &PyNumberMethods::nb_multiply, "*");
    if (result != Py_NotImplemented) {
        return result;
    }

    auto mv = Py_TYPE(v)->tp_as_sequence;
    auto mw = Py_TYPE(w)->tp_as_sequence;
    if (mv && mv->sq_repeat) {
        return sequence_repeat(mv->sq_repeat, v, w);
    } else if (mw && mw->sq_repeat) {
        return sequence_repeat(mw->sq_repeat, w, v);
    }
    binop_type_error(v, w, "*");
}


PyObject *handle_INPLACE_MULTIPLY(PyObject *v, PyObject *w) {
    auto result = binary_iop_impl(v, w, &PyNumberMethods::nb_inplace_multiply, &PyNumberMethods::nb_multiply, "*=");
    if (result != Py_NotImplemented) {
        return result;
    }

    PySequenceMethods *mv = Py_TYPE(v)->tp_as_sequence;
    PySequenceMethods *mw = Py_TYPE(w)->tp_as_sequence;
    if (mv) {
        auto f = mv->sq_inplace_repeat ? mv->sq_inplace_repeat : mv->sq_repeat;
        if (f) {
            return sequence_repeat(f, v, w);
        }
    } else if (mw && mw->sq_repeat) {
        /* Note that the right hand operand should not be
         * mutated in this case so sq_inplace_repeat is not
         * used. */
        return sequence_repeat(mw->sq_repeat, w, v);
    }
    binop_type_error(v, w, "*=");
}

PyObject *handle_BINARY_FLOOR_DIVIDE(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"//"};
    return binary_op<&PyNumberMethods::nb_floor_divide, op_sign>(v, w);
}

PyObject *handle_INPLACE_FLOOR_DIVIDE(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"//="};
    return binary_iop<&PyNumberMethods::nb_inplace_floor_divide, &PyNumberMethods::nb_floor_divide, op_sign>(v, w);
}

PyObject *handle_BINARY_TRUE_DIVIDE(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"/"};
    return binary_op<&PyNumberMethods::nb_true_divide, op_sign>(v, w);
}

PyObject *handle_INPLACE_TRUE_DIVIDE(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"/="};
    return binary_iop<&PyNumberMethods::nb_inplace_true_divide, &PyNumberMethods::nb_true_divide, op_sign>(v, w);
}

PyObject *handle_BINARY_MODULO(PyObject *v, PyObject *w) {
    if (PyUnicode_CheckExact(v) && (PyUnicode_CheckExact(w) || !PyUnicode_Check(w))) {
        // fast path; string formatting, but not if the RHS is a str subclass (see issue28598)
        auto res = PyUnicode_Format(v, w);
        gotoErrorHandler(!res);
        return res;
    } else {
        static constexpr char op_sign[]{"%"};
        return binary_op<&PyNumberMethods::nb_remainder, op_sign>(v, w);
    }
}

PyObject *handle_INPLACE_MODULO(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"%="};
    return binary_iop<&PyNumberMethods::nb_inplace_remainder, &PyNumberMethods::nb_remainder, op_sign>(v, w);
}

PyObject *handle_BINARY_POWER(PyObject *v, PyObject *w) {
    // TODO: 简化
    auto res = PyNumber_Power(v, w, Py_None);
    gotoErrorHandler(!res);
    return res;
}

PyObject *handle_INPLACE_POWER(PyObject *v, PyObject *w) {
    auto res = PyNumber_InPlacePower(v, w, Py_None);
    gotoErrorHandler(!res);
    return res;
}

PyObject *handle_BINARY_MATRIX_MULTIPLY(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"@"};
    return binary_op<&PyNumberMethods::nb_matrix_multiply, op_sign>(v, w);
}

PyObject *handle_INPLACE_MATRIX_MULTIPLY(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"@="};
    return binary_iop<&PyNumberMethods::nb_inplace_matrix_multiply, &PyNumberMethods::nb_matrix_multiply, op_sign>(v, w);
}

PyObject *handle_BINARY_LSHIFT(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"<<"};
    return binary_op<&PyNumberMethods::nb_lshift, op_sign>(v, w);
}

PyObject *handle_INPLACE_LSHIFT(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"<<="};
    return binary_iop<&PyNumberMethods::nb_inplace_lshift, &PyNumberMethods::nb_lshift, op_sign>(v, w);
}

PyObject *handle_BINARY_RSHIFT(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{">>"};
    return binary_op<&PyNumberMethods::nb_rshift, op_sign>(v, w);
}

PyObject *handle_INPLACE_RSHIFT(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{">>="};
    return binary_iop<&PyNumberMethods::nb_inplace_rshift, &PyNumberMethods::nb_rshift, op_sign>(v, w);
}

PyObject *handle_BINARY_AND(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"&"};
    return binary_op<&PyNumberMethods::nb_and, op_sign>(v, w);
}

PyObject *handle_INPLACE_AND(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"&="};
    return binary_iop<&PyNumberMethods::nb_inplace_and, &PyNumberMethods::nb_and, op_sign>(v, w);
}

PyObject *handle_BINARY_OR(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"|"};
    return binary_op<&PyNumberMethods::nb_or, op_sign>(v, w);
}

PyObject *handle_INPLACE_OR(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"|="};
    return binary_iop<&PyNumberMethods::nb_inplace_or, &PyNumberMethods::nb_or, op_sign>(v, w);
}

PyObject *handle_BINARY_XOR(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"^"};
    return binary_op<&PyNumberMethods::nb_xor, op_sign>(v, w);
}

PyObject *handle_INPLACE_XOR(PyObject *v, PyObject *w) {
    static constexpr char op_sign[]{"^="};
    return binary_iop<&PyNumberMethods::nb_inplace_xor, &PyNumberMethods::nb_xor, op_sign>(v, w);
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

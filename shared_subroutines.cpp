#include <iostream>

#include <Python.h>

#undef HAVE_STD_ATOMIC

#include <opcode.h>
#include <frameobject.h>
#include <internal/pycore_pystate.h>
#include <internal/pycore_code.h>
#include <internal/pycore_pyerrors.h>
#include <internal/pycore_abstract.h>
#include <internal/pycore_interp.h>
#include <internal/pycore_object.h>

#include "shared_symbols.h"
#include "general_utilities.h"

using namespace std;

// 不需要，反正c++20了
#ifdef __has_cpp_attribute
#if __has_cpp_attribute(likely) && __has_cpp_attribute(unlikely)
#define LIKLEY [[likely]]
#define UNLIKLEY [[unlikely]]
#endif
#endif

static PyObject *const python_bool_values[]{Py_False, Py_True};
static _Py_Identifier PyId___name__{"__name__", -1};

void handle_dealloc(PyObject *obj) {
#ifdef Py_TRACE_REFS
    _Py_ForgetReference(op);
#endif
    Py_TYPE(obj)->tp_dealloc(obj);
}

void handle_INCREF(PyObject *obj) {
    Py_INCREF(obj);
}

void handle_DECREF(PyObject *obj) {
    Py_DECREF(obj);
}

void handle_XDECREF(PyObject *obj) {
    Py_XDECREF(obj);
}

// TODO: 在想，能不能设计俩版本，decref在这里实现
// TODO: 能否设置hot inline等确保展开

[[noreturn]] static void gotoUnwind(PyThreadState *tstate, PyFrameObject *f) {
    auto cframe = static_cast<ExtendedCFrame *>(tstate->cframe);
    f->f_state = FRAME_UNWINDING;
    ptrdiff_t handler = -1;

    while (f->f_iblock > 0) {
        /* Pop the current block. */
        PyTryBlock *b = &f->f_blockstack[--f->f_iblock];

        if (b->b_type == EXCEPT_HANDLER) {
            PyObject *type, *value, *traceback;
            while (f->f_stackdepth > b->b_level + 3) {
                Py_XDECREF(f->f_valuestack[--f->f_stackdepth]);
            }
            auto exc_info = tstate->exc_info;
            type = exc_info->exc_type;
            value = exc_info->exc_value;
            traceback = exc_info->exc_traceback;
            exc_info->exc_type = f->f_valuestack[--f->f_stackdepth];
            exc_info->exc_value = f->f_valuestack[--f->f_stackdepth];
            exc_info->exc_traceback = f->f_valuestack[--f->f_stackdepth];
            Py_XDECREF(type);
            Py_XDECREF(value);
            Py_XDECREF(traceback);
            continue;
        }
        while (f->f_stackdepth > b->b_level) {
            PyObject *v = f->f_valuestack[--f->f_stackdepth];
            Py_XDECREF(v);
        }
        assert(b->b_type == SETUP_FINALLY);
        if (b->b_type == SETUP_FINALLY) {
            PyObject *exc, *val, *tb;
            handler = b->b_handler;
            _PyErr_StackItem *exc_info = tstate->exc_info;
            /* Beware, this invalidates all b->b_* fields */
            PyFrame_BlockSetup(f, EXCEPT_HANDLER, f->f_lasti, f->f_stackdepth);
            f->f_valuestack[f->f_stackdepth++] = exc_info->exc_traceback;
            f->f_valuestack[f->f_stackdepth++] = exc_info->exc_value;
            if (exc_info->exc_type) {
                f->f_valuestack[f->f_stackdepth++] = exc_info->exc_type;
            } else {
                Py_INCREF(Py_None);
                f->f_valuestack[f->f_stackdepth++] = Py_None;
            }
            _PyErr_Fetch(tstate, &exc, &val, &tb);
            /* Make the raw exception data
               available to the handler,
               so a program can emulate the
               Python main loop. */
            _PyErr_NormalizeException(tstate, &exc, &val, &tb);
            if (tb) {
                PyException_SetTraceback(val, tb);
            } else {
                PyException_SetTraceback(val, Py_None);
            }
            Py_INCREF(exc);
            exc_info->exc_type = exc;
            Py_INCREF(val);
            exc_info->exc_value = val;
            exc_info->exc_traceback = tb;
            if (!tb) {
                tb = Py_None;
            }
            Py_INCREF(tb);
            f->f_valuestack[f->f_stackdepth++] = tb;
            f->f_valuestack[f->f_stackdepth++] = val;
            f->f_valuestack[f->f_stackdepth++] = exc;
            /* Resume normal execution */
            f->f_state = FRAME_EXECUTING;
            break;
        }
    }

    if (handler >= 0) {
        f->f_blockstack[CO_MAXBLOCKS - 1].b_handler = handler;
        longjmp(cframe->frame_jmp_buf, 1);
    } else {
        while (f->f_stackdepth) {
            PyObject *v = f->f_valuestack[--f->f_stackdepth];
            Py_XDECREF(v);
        }
        f->f_state = FRAME_RAISED;
        longjmp(cframe->frame_jmp_buf, 2);
    }
}

static auto getStackDepth(PyThreadState *tstate, PyFrameObject *frame) {
    assert(tstate->frame == frame);
    return static_cast<ExtendedCFrame *>(tstate->cframe)->sp_map[frame->f_lasti];
}

[[noreturn]] static void gotoErrorHandler(PyThreadState *tstate) {
    assert(_PyErr_Occurred(tstate));
    auto f = tstate->frame;
    PyTraceBack_Here(f);
    assert(!tstate->c_tracefunc); // TODO: 要不要支持它
    f->f_stackdepth = getStackDepth(tstate, f);
    gotoUnwind(tstate, f);
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
        static _Py_Identifier PyId_name{"name", -1};
        _PyObject_SetAttrId(value, &PyId_name, name);
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
    auto py_instr = PyInstrPointer(code);
    auto current_instr = py_instr + frame->f_lasti;
    auto opcode = current_instr.opcode();
    auto oparg = current_instr.oparg(py_instr);
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
    } else if (opcode == GEN_START) {
        if (oparg > 2) {
            _PyErr_SetString(tstate, PyExc_SystemError, "Illegal kind for GEN_START");
        } else {
            static const char *gen_kind[3] = {"generator", "coroutine", "async generator"};
            _PyErr_Format(tstate, PyExc_TypeError,
                    "can't send non-None value to a just-started %s", gen_kind[oparg]);
        }
    } else {
        assert(opcode == MAKE_FUNCTION || opcode == YIELD_FROM);
    }
    gotoErrorHandler(tstate);
}

PyObject *handle_LOAD_CLASSDEREF(PyFrameObject *f, Py_ssize_t oparg) {
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
    auto hash = reinterpret_cast<PyASCIIObject *>(name)->hash;
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

static PyObject mark_as_not_method{
        _PyObject_EXTRA_INIT
        1,
        &_PyNone_Type
};

void handle_LOAD_METHOD(PyObject *name, PyObject **sp) {
    PyObject *obj = sp[0];
    PyObject *meth = nullptr;
    int meth_found = _PyObject_GetMethod(obj, name, &meth);
    gotoErrorHandler(!meth);
    if (meth_found) {
        sp[0] = meth;
        sp[1] = obj;
    } else {
        Py_INCREF(&mark_as_not_method);
        sp[0] = &mark_as_not_method;
        sp[1] = meth;
        Py_DECREF(obj);
    }
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

// TODO: 删了
static void checkSlotResult(PyObject *obj, const char *slot_name, bool result) {
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
static void checkSlotResult(PyObject *obj, T PyNumberMethods::* op_slot, bool result) {
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

    if constexpr (sizeof...(Ts)) {
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


bool handle_CONTAINS_OP(PyObject *container, PyObject *value) {
    auto sqm = Py_TYPE(container)->tp_as_sequence;
    Py_ssize_t res;
    if (sqm && sqm->sq_contains) {
        res = sqm->sq_contains(container, value);
    } else {
        res = _PySequence_IterSearch(container, value, PY_ITERSEARCH_CONTAINS);
    }
    gotoErrorHandler(res < 0);
    return res > 0;
}

bool castPyObjectToBool(PyObject *o) {
    if (o == Py_None) {
        return false;
    }
    auto type = Py_TYPE(o);
    Py_ssize_t res;
    if (type->tp_as_number && type->tp_as_number->nb_bool) {
        res = type->tp_as_number->nb_bool(o);
    } else if (type->tp_as_mapping && type->tp_as_mapping->mp_length) {
        res = type->tp_as_mapping->mp_length(o);
    } else if (type->tp_as_sequence && type->tp_as_sequence->sq_length) {
        res = type->tp_as_sequence->sq_length(o);
    } else {
        return true;
    }
    gotoErrorHandler(res < 0);
    return res > 0;
}

PyObject *handle_GET_ITER(PyObject *o) {
    auto type = Py_TYPE(o);
    if (type->tp_iter) {
        auto *res = type->tp_iter(o);
        gotoErrorHandler(!res);
        auto res_type = Py_TYPE(res);
        if (res_type->tp_iternext && res_type->tp_iternext != &_PyObject_NextNotImplemented) {
            return res;
        } else {
            PyErr_Format(PyExc_TypeError, "iter() returned non-iterator of type '%.100s'", res_type->tp_name);
            Py_DECREF(res);
            gotoErrorHandler();
        }
    } else {
        if (!PyDict_Check(o) && type->tp_as_sequence && type->tp_as_sequence->sq_item) {
            auto res = PySeqIter_New(o);
            gotoErrorHandler(!res);
            return res;
        }
        PyErr_Format(PyExc_TypeError, "'%.200s' object is not iterable", type->tp_name);
        gotoErrorHandler();
    }
}

static auto makeFunctionCall(PyObject *func_args[], Py_ssize_t nargs, Py_ssize_t decref, PyObject *kwnames = nullptr) {
    auto ret = PyObject_Vectorcall(func_args[0], func_args + 1, nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
    gotoErrorHandler(!ret);
    do {
        Py_DECREF(func_args[nargs]);
    } while (nargs--);
    return ret;
}

PyObject *handle_CALL_FUNCTION(PyObject **func_args, Py_ssize_t nargs) {
    return makeFunctionCall(func_args, nargs, nargs);
}

PyObject *handle_CALL_METHOD(PyObject **func_args, Py_ssize_t nargs) {
    bool is_meth = func_args[0] != &mark_as_not_method;
    func_args += !is_meth;
    nargs += is_meth;
    auto ret = makeFunctionCall(func_args, nargs, nargs);
    _Py_SET_REFCNT(&mark_as_not_method, _Py_REFCNT(&mark_as_not_method) - !is_meth);
    return ret;
}

PyObject *handle_CALL_FUNCTION_KW(PyObject **func_args, Py_ssize_t nargs) {
    auto kwargs = func_args[nargs + 1];
    return makeFunctionCall(func_args, nargs - PyTuple_GET_SIZE(kwargs), nargs + 1, kwargs);
}

static void formatFunctionCallError(PyThreadState *tstate, PyObject *func, const char *error, auto... args) {
    PyObject *funcstr = _PyObject_FunctionStr(func);
    if (funcstr) {
        _PyErr_Format(tstate, PyExc_TypeError, error, funcstr, args...);
        Py_DECREF(funcstr);
    }
}

PyObject *handle_CALL_FUNCTION_EX(PyObject *func, PyObject *args, PyObject *kwargs) {
    assert(!kwargs || PyDict_CheckExact(kwargs));
    PyObject *ret = nullptr;
    if (PyTuple_CheckExact(args)) {
        ret = PyObject_Call(func, kwargs, kwargs);
    } else {
        auto t = PySequence_Tuple(args);
        if (!t) [[unlikely]] {
            auto tstate = _PyThreadState_GET();
            if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError)) {
                _PyErr_Clear(tstate);
                formatFunctionCallError(tstate, func,
                        "%U argument after ** must be a mapping, not %.200s", Py_TYPE(kwargs)->tp_name);
            }
            gotoErrorHandler(tstate);
        }
        ret = PyObject_Call(func, t, kwargs);
        Py_DECREF(t);
    }
    gotoErrorHandler(!ret);
    return ret;
}

void handle_FOR_ITER() {
    auto tstate = _PyThreadState_GET();
    if (_PyErr_Occurred(tstate)) {
        if (!_PyErr_ExceptionMatches(tstate, PyExc_StopIteration)) {
            gotoErrorHandler(tstate);
        }
        // maybe we should support trace
        _PyErr_Clear(tstate);
    }
}

PyObject *handle_BUILD_STRING(PyObject **arr, Py_ssize_t num) {
    auto str = PyUnicode_New(0, 0);
    gotoErrorHandler(!str);
    str = _PyUnicode_JoinArray(str, arr, num);
    gotoErrorHandler(!str);
    while (--num >= 0) {
        Py_DECREF(arr[num]);
    }
    return str;
}

PyObject *handle_BUILD_TUPLE(PyObject **arr, Py_ssize_t num) {
    auto tup = PyTuple_New(num);
    gotoErrorHandler(!tup);
    while (--num >= 0) {
        PyTuple_SET_ITEM(tup, num, arr[num]);
    }
    return tup;
}

PyObject *handle_BUILD_LIST(PyObject **arr, Py_ssize_t num) {
    auto list = PyList_New(num);
    gotoErrorHandler(!list);
    while (--num >= 0) {
        PyList_SET_ITEM(list, num, arr[num]);
    }
    return list;
}

PyObject *handle_BUILD_SET(PyObject **arr, Py_ssize_t num) {
    auto *set = PySet_New(nullptr);
    gotoErrorHandler(!set);
    for (auto i = 0; i < num; i++) {
        if (PySet_Add(set, arr[i])) [[unlikely]] {
            Py_DECREF(set);
            gotoErrorHandler();
        }
    }
    for (auto i = 0; i < num; i++) {
        Py_DECREF(arr[i]);
    }
    return set;
}

PyObject *handle_BUILD_MAP(PyObject **arr, Py_ssize_t num) {
    auto map = _PyDict_NewPresized(num);
    gotoErrorHandler(!map);
    for (auto i = 0; i < num; i++) {
        if (PyDict_SetItem(map, arr[2 * i], arr[2 * i + 1])) [[unlikely]] {
            Py_DECREF(map);
            gotoErrorHandler();
        }
    }
    for (auto i = 0; i < num; i++) {
        Py_DECREF(arr[2 * i]);
        Py_DECREF(arr[2 * i + 1]);
    }
    return map;
}

PyObject *handle_BUILD_CONST_KEY_MAP(PyObject **arr, Py_ssize_t num) {
    auto keys = arr[num];
    if (!PyTuple_CheckExact(keys) || PyTuple_GET_SIZE(keys) != num) {
        auto tstate = _PyThreadState_GET();
        _PyErr_SetString(tstate, PyExc_SystemError, "bad BUILD_CONST_KEY_MAP keys argument");
        gotoErrorHandler(tstate);
    }
    auto map = _PyDict_NewPresized(num);
    gotoErrorHandler(!map);
    for (auto i = 0; i < num; i++) {
        if (PyDict_SetItem(map, PyTuple_GET_ITEM(keys, i), arr[i])) [[unlikely]] {
            Py_DECREF(map);
            gotoErrorHandler();
        }
    }
    Py_DECREF(keys);
    for (auto i = 0; i < num; i++) {
        Py_DECREF(arr[i]);
    }
    return map;
}

void handle_LIST_APPEND(PyObject *list, PyObject *value) {
    gotoErrorHandler(PyList_Append(list, value));
}

void handle_SET_ADD(PyObject *set, PyObject *value) {
    gotoErrorHandler(PySet_Add(set, value));
}

void handle_MAP_ADD(PyObject *map, PyObject *key, PyObject *value) {
    gotoErrorHandler(PyDict_SetItem(map, key, value));
}

void handle_LIST_EXTEND(PyObject *list, PyObject *iterable) {
    PyObject *none_val = _PyList_Extend((PyListObject *) list, iterable);
    if (none_val) [[likely]] {
        Py_DECREF(none_val);
        return;
    }
    auto tstate = _PyThreadState_GET();
    if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError)
            && !Py_TYPE(iterable)->tp_iter && !PySequence_Check(iterable)) {
        _PyErr_Clear(tstate);
        _PyErr_Format(tstate, PyExc_TypeError,
                "Value after * must be an iterable, not %.200s",
                Py_TYPE(iterable)->tp_name);
    }
    gotoErrorHandler(tstate);
}

void handle_SET_UPDATE(PyObject *set, PyObject *iterable) {
    gotoErrorHandler(_PySet_Update(set, iterable) < 0);
}

void handle_DICT_UPDATE(PyObject *dict, PyObject *update) {
    if (PyDict_Update(dict, update) >= 0) [[likely]] {
        return;
    }
    auto tstate = _PyThreadState_GET();
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        _PyErr_Format(tstate, PyExc_TypeError, "'%.200s' object is not a mapping", Py_TYPE(update)->tp_name);
    }
    gotoErrorHandler(tstate);
}

PyObject *handle_LIST_TO_TUPLE(PyObject *list) {
    auto tuple = PyList_AsTuple(list);
    gotoErrorHandler(!tuple);
    return tuple;
}

void handle_DICT_MERGE(PyObject *func, PyObject *dict, PyObject *update) {
    if (_PyDict_MergeEx(dict, update, 2) == 0) [[likely]] {
        return;
    }
    auto tstate = _PyThreadState_GET();
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        _PyErr_Clear(tstate);
        formatFunctionCallError(tstate, func,
                "%U argument after ** must be a mapping, not %.200s", Py_TYPE(update)->tp_name);
    } else if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
        auto val = tstate->curexc_value;
        if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
            PyObject *key = PyTuple_GET_ITEM(val, 0);
            Py_INCREF(key);
            _PyErr_Clear(tstate);
            formatFunctionCallError(tstate, func, "%U got multiple values for keyword argument '%S'", key);
            Py_DECREF(key);
        }
    }
    gotoErrorHandler(tstate);
}

PyObject *handle_FORMAT_VALUE(PyObject *value, PyObject *fmt_spec, int which_conversion) {
    switch (which_conversion) {
    case FVC_NONE:
        Py_INCREF(value);
        break;
    case FVC_STR:
        gotoErrorHandler(!(value = PyObject_Str(value)));
        break;
    case FVC_REPR:
        gotoErrorHandler(!(value = PyObject_Repr(value)));
        break;
    case FVC_ASCII:
        gotoErrorHandler(!(value = PyObject_ASCII(value)));
        break;
    default: {
        auto tstate = _PyThreadState_GET();
        _PyErr_Format(tstate, PyExc_SystemError, "unexpected conversion flag %d", which_conversion);
        gotoErrorHandler(tstate);
    }
    }

    if (!fmt_spec && PyUnicode_CheckExact(value)) {
        return value;
    }
    auto fmt_value = PyObject_Format(value, fmt_spec);
    Py_DECREF(value);
    gotoErrorHandler(!fmt_value);
    return fmt_value;
}

PyObject *handle_BUILD_SLICE(PyObject *start, PyObject *stop, PyObject *step) {
    auto slice = PySlice_New(start, stop, step);
    gotoErrorHandler(!slice);
    return slice;
}

/* Logic for the raise statement (too complicated for inlining).
   This *consumes* a reference count to each of its arguments. */
void handle_RAISE_VARARGS(PyObject *cause, PyObject *exc) {
    auto tstate = _PyThreadState_GET();
    if (!exc) {
        auto exc_info = _PyErr_GetTopmostException(tstate);
        if (Py_IsNone(exc_info->exc_type) || !exc_info->exc_type) {
            _PyErr_SetString(tstate, PyExc_RuntimeError, "No active exception to reraise");
            gotoErrorHandler(tstate);
        }
        Py_INCREF(exc_info->exc_type);
        Py_XINCREF(exc_info->exc_value);
        Py_XINCREF(exc_info->exc_traceback);
        _PyErr_Restore(tstate, exc_info->exc_type, exc_info->exc_value, exc_info->exc_traceback);
        gotoErrorHandler(tstate);
    }

    PyObject *type = nullptr, *value = nullptr;
    if (PyExceptionClass_Check(exc)) {
        value = _PyObject_CallNoArg(exc);
        gotoErrorHandler(!value, tstate);
        if (!PyExceptionInstance_Check(value)) {
            _PyErr_Format(tstate, PyExc_TypeError,
                    "calling %R should have returned an instance of BaseException, not %R",
                    type, Py_TYPE(value));
            Py_DECREF(value);
            gotoErrorHandler(tstate);
        }
        type = exc;
    } else if (PyExceptionInstance_Check(exc)) {
        value = exc;
        Py_INCREF(value);
        type = PyExceptionInstance_Class(exc);
    } else {
        /* Not something you can raise.  You get an exception anyway, just not what you specified :-) */
        _PyErr_SetString(tstate, PyExc_TypeError, "exceptions must derive from BaseException");
        gotoErrorHandler(tstate);
    }

    if (cause) {
        if (PyExceptionClass_Check(cause)) {
            auto fixed_cause = _PyObject_CallNoArg(cause);
            if (!fixed_cause) {
                Py_DECREF(value);
                gotoErrorHandler(tstate);
            }
            PyException_SetCause(value, fixed_cause);
        } else if (PyExceptionInstance_Check(cause)) {
            Py_INCREF(cause);
            PyException_SetCause(value, cause);
        } else if (Py_IsNone(cause)) {
            PyException_SetCause(value, nullptr);
        } else {
            _PyErr_SetString(tstate, PyExc_TypeError, "exception causes must derive from BaseException");
            Py_DECREF(value);
            gotoErrorHandler(tstate);
        }
    }

    _PyErr_SetObject(tstate, type, value);
    /* _PyErr_SetObject incref's its arguments */
    Py_DECREF(value);
    gotoErrorHandler(tstate);
}

void handle_SETUP_ANNOTATIONS(PyFrameObject *f) {
    _Py_IDENTIFIER(__annotations__);
    if (!f->f_locals) {
        auto tstate = _PyThreadState_GET();
        _PyErr_Format(tstate, PyExc_SystemError, "no locals found when setting up annotations");
        gotoErrorHandler(tstate);
    }
    /* check if __annotations__ in locals()... */
    if (PyDict_CheckExact(f->f_locals)) {
        auto ann_dict = _PyDict_GetItemIdWithError(f->f_locals, &PyId___annotations__);
        if (!ann_dict) {
            auto tstate = _PyThreadState_GET();
            gotoErrorHandler(_PyErr_Occurred(tstate));
            /* ...if not, create a new one */
            ann_dict = PyDict_New();
            gotoErrorHandler(!ann_dict);
            auto err = _PyDict_SetItemId(f->f_locals, &PyId___annotations__, ann_dict);
            Py_DECREF(ann_dict);
            gotoErrorHandler(err);
        }
    } else {
        /* do the same if locals() is not a dict */
        PyObject *ann_str = _PyUnicode_FromId(&PyId___annotations__);
        gotoErrorHandler(!ann_str);
        auto ann_dict = PyObject_GetItem(f->f_locals, ann_str);
        if (!ann_dict) {
            auto tstate = _PyThreadState_GET();
            gotoErrorHandler(!_PyErr_ExceptionMatches(tstate, PyExc_KeyError));
            _PyErr_Clear(tstate);
            ann_dict = PyDict_New();
            gotoErrorHandler(!ann_dict);
            auto err = PyObject_SetItem(f->f_locals, ann_str, ann_dict);
            Py_DECREF(ann_dict);
            gotoErrorHandler(err);
        } else {
            Py_DECREF(ann_dict);
        }
    }
}

void handle_PRINT_EXPR(PyObject *value) {
    _Py_IDENTIFIER(displayhook);
    PyObject *hook = _PySys_GetObjectId(&PyId_displayhook);
    if (!hook) {
        auto tstate = _PyThreadState_GET();
        _PyErr_SetString(tstate, PyExc_RuntimeError, "lost sys.displayhook");
        gotoErrorHandler(tstate);
    }
    auto res = PyObject_CallOneArg(hook, value);
    gotoErrorHandler(!res);
    Py_DECREF(res);
}

void handle_UNPACK_SEQUENCE(PyObject *seq, Py_ssize_t num, PyObject **dest) {
    if (PyTuple_CheckExact(seq) && PyTuple_GET_SIZE(seq) == num) {
        auto items = (reinterpret_cast<PyTupleObject *>(seq))->ob_item;
        while (num--) {
            auto item = items[num];
            Py_INCREF(item);
            *dest++ = item;
        }
    } else if (PyList_CheckExact(seq) && PyList_GET_SIZE(seq) == num) {
        auto items = (reinterpret_cast<PyListObject *>(seq))->ob_item;
        while (num--) {
            auto item = items[num];
            Py_INCREF(item);
            *dest++ = item;
        }
    } else {
        handle_UNPACK_EX(seq, num, -1, dest + num);
    }
}

void handle_UNPACK_EX(PyObject *seq, Py_ssize_t before_star, Py_ssize_t after_star, PyObject **ptr) {
    assert(seq);
    PyObject **ptr_end = ptr;
    auto tstate = _PyThreadState_GET();
    auto iter = PyObject_GetIter(seq);
    const auto &clear_and_exit = [&]() {
        Py_DECREF(iter);
        while (ptr != ptr_end) {
            Py_DECREF(*ptr++);
        }
        gotoErrorHandler(tstate);
    };

    if (!iter) {
        if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) && !Py_TYPE(seq)->tp_iter && !PySequence_Check(seq)) {
            _PyErr_Format(tstate, PyExc_TypeError,
                    "cannot unpack non-iterable %.200s object",
                    Py_TYPE(seq)->tp_name);
        }
        gotoErrorHandler();
    }

    for (int i = 0; i < before_star; i++) {
        if (auto next = PyIter_Next(iter)) {
            *--ptr = next;
            continue;
        }
        /* Iterator done, via error or exhaustion. */
        if (!_PyErr_Occurred(tstate)) {
            if (after_star == -1) {
                _PyErr_Format(tstate, PyExc_ValueError,
                        "not enough values to unpack (expected %d, got %d)", before_star, i);
            } else {
                _PyErr_Format(tstate, PyExc_ValueError,
                        "not enough values to unpack (expected at least %d, got %d)", before_star + after_star, i);
            }
        }
        clear_and_exit();
    }

    if (after_star == -1) {
        /* We better have exhausted the iterator now. */
        auto w = PyIter_Next(iter);
        if (w) {
            Py_DECREF(w);
            _PyErr_Format(tstate, PyExc_ValueError, "too many values to unpack (expected %d)", before_star);
            clear_and_exit();
        }
        if (_PyErr_Occurred(tstate)) {
            clear_and_exit();
        }
        Py_DECREF(iter);
        return;
    }

    auto l = PySequence_List(iter);
    if (!l) {
        clear_and_exit();
    }
    *--ptr = l;

    auto list_size = PyList_GET_SIZE(l);
    if (list_size < after_star) {
        _PyErr_Format(tstate, PyExc_ValueError,
                "not enough values to unpack (expected at least %d, got %zd)",
                before_star + after_star, before_star + list_size);
        clear_and_exit();
    }

    /* Pop the "after-variable" args off the list. */
    for (int j = after_star; j > 0; j--) {
        *--ptr = PyList_GET_ITEM(l, list_size - j);
    }
    /* Resize the list. */
    Py_SET_SIZE(l, list_size - after_star);
    Py_DECREF(iter);
}

PyObject *hanlde_GET_LEN(PyObject *value) {
    auto len_i = PyObject_Length(value);
    gotoErrorHandler(len_i < 0);
    auto len_o = PyLong_FromSsize_t(len_i);
    gotoErrorHandler(!len_o);
    return len_o;
}

void hanlde_MATCH_KEYS(PyObject *map, PyObject *keys, PyObject **result) {
    static PyObject dummy = {_PyObject_EXTRA_INIT 1, &PyBaseObject_Type};

    assert(PyTuple_CheckExact(keys));
    Py_ssize_t nkeys = PyTuple_GET_SIZE(keys);
    if (!nkeys) {
        result[0] = PyTuple_New(0);;
        result[1] = Py_NewRef(Py_True);
        return;
    }
    PyObject *get = nullptr;
    PyObject *seen = nullptr;
    PyObject *values = nullptr;

    _Py_IDENTIFIER(get);
    if (!(get = _PyObject_GetAttrId(map, &PyId_get))) [[unlikely]] {
        goto fail;
    }
    if (!(seen = PySet_New(nullptr))) [[unlikely]] {
        goto fail;
    }
    if (!(values = PyList_New(0))) [[unlikely]] {
        goto fail;
    }
    for (auto i : IntRange(nkeys)) {
        auto key = PyTuple_GET_ITEM(keys, i);
        if (PySet_Contains(seen, key) || PySet_Add(seen, key)) [[unlikely]] {
            auto tstate = _PyThreadState_GET();
            if (!_PyErr_Occurred(tstate)) {
                _PyErr_Format(tstate, PyExc_ValueError, "mapping pattern checks duplicate key (%R)", key);
            }
            goto fail;
        }
        auto value = PyObject_CallFunctionObjArgs(get, key, &dummy, nullptr);
        if (!value) [[unlikely]] {
            goto fail;
        }
        if (value == &dummy) {
            Py_DECREF(value);
            Py_DECREF(get);
            Py_DECREF(seen);
            Py_DECREF(values);
            result[0] = Py_NewRef(Py_None);
            result[1] = Py_NewRef(Py_False);
            return;
        }
        PyList_Append(values, value);
        Py_DECREF(value);
    }
    Py_SETREF(values, PyList_AsTuple(values));
    Py_DECREF(get);
    Py_DECREF(seen);
    result[0] = values;
    result[1] = Py_NewRef(Py_True);
    return;

fail:
    Py_XDECREF(get);
    Py_XDECREF(seen);
    Py_XDECREF(values);
    gotoErrorHandler();
}

void hanlde_MATCH_CLASS(Py_ssize_t nargs, PyObject *kwargs, PyObject **result) {
    auto tstate = _PyThreadState_GET();
    PyObject *seen = nullptr;
    PyObject *attrs = nullptr;
    PyObject *match_args = nullptr;
    auto subject = result[-2];
    auto type = result[-1];
    auto type_ = reinterpret_cast<PyTypeObject *>(type);
    assert(PyTuple_CheckExact(kwargs));

    const auto &match_class_attr = [&](PyObject *name) {
        if (PySet_Contains(seen, name) || PySet_Add(seen, name)) {
            if (!_PyErr_Occurred(tstate)) {
                _PyErr_Format(tstate, PyExc_TypeError,
                        "%s() got multiple sub-patterns for attribute %R", type_->tp_name, name);
            }
            return false;
        }
        auto attr = PyObject_GetAttr(subject, name);
        if (attr) {
            PyList_Append(attrs, attr);
            Py_DECREF(attr);
            return true;
        }
        if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
            _PyErr_Clear(tstate);
        }
        return false;
    };

    if (!PyType_Check(type)) {
        _PyErr_Format(tstate, PyExc_TypeError, "called match pattern must be a type");
        goto fail;
    }
    // First, an isinstance check:
    if (PyObject_IsInstance(subject, type) <= 0) {
        goto fail;
    }
    // So far so good:
    if (!(seen = PySet_New(nullptr))) {
        goto fail;
    }
    if (!(attrs = PyList_New(0))) {
        goto fail;
    }

    // NOTE: From this point on, goto fail on failure:
    // First, the positional subpatterns:
    if (nargs) {
        bool match_self = false;
        match_args = PyObject_GetAttrString(type, "__match_args__");
        if (match_args) {
            if (!PyTuple_CheckExact(match_args)) {
                _PyErr_Format(tstate, PyExc_TypeError, "%s.__match_args__ must be a tuple (got %s)",
                        type_->tp_name,
                        Py_TYPE(match_args)->tp_name);
                goto fail;
            }
        } else if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
            _PyErr_Clear(tstate);
            // _Py_TPFLAGS_MATCH_SELF is only acknowledged if the type does not
            // define __match_args__. This is natural behavior for subclasses:
            // it's as if __match_args__ is some "magic" value that is lost as
            // soon as they redefine it.
            match_args = PyTuple_New(0);
            match_self = PyType_HasFeature(type_, _Py_TPFLAGS_MATCH_SELF);
        } else {
            goto fail;
        }
        assert(PyTuple_CheckExact(match_args));
        auto allowed = match_self ? 1 : PyTuple_GET_SIZE(match_args);
        if (allowed < nargs) {
            _PyErr_Format(tstate, PyExc_TypeError,
                    "%s() accepts %d positional sub-pattern%s (%d given)",
                    type_->tp_name,
                    allowed, (allowed == 1) ? "" : "s", nargs);
            goto fail;
        }
        if (match_self) {
            // Easy. Copy the subject itself, and move on to kwargs.
            PyList_Append(attrs, subject);
        } else {
            for (auto i : IntRange(nargs)) {
                PyObject *name = PyTuple_GET_ITEM(match_args, i);
                if (!PyUnicode_CheckExact(name)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                            "__match_args__ elements must be strings (got %s)", Py_TYPE(name)->tp_name);
                    goto fail;
                }
                if (!match_class_attr(name)) {
                    goto fail;
                }
            }
        }
        Py_CLEAR(match_args);
    }

    // Finally, the keyword subpatterns:
    for (auto i : IntRange(PyTuple_GET_SIZE(kwargs))) {
        PyObject *name = PyTuple_GET_ITEM(kwargs, i);
        if (!match_class_attr(name)) {
            goto fail;
        }
    }
    Py_SETREF(attrs, PyList_AsTuple(attrs));
    Py_DECREF(seen);
    result[-2] = attrs;
    result[-1] = Py_NewRef(Py_True);
    Py_DECREF(subject);
    Py_DECREF(type);
    return;

fail:
    // We really don't care whether an error was raised or not... that's our
    // caller's problem. All we know is that the match failed.
    Py_XDECREF(seen);
    Py_XDECREF(attrs);
    Py_XDECREF(match_args);
    gotoErrorHandler(_PyErr_Occurred(tstate), tstate);
    result[-1] = Py_NewRef(Py_False);
    Py_DECREF(type);
}

PyObject *handle_COPY_DICT_WITHOUT_KEYS(PyObject *subject, PyObject *keys) {
    PyObject *rest = PyDict_New();
    gotoErrorHandler(!rest);
    if (PyDict_Update(rest, subject)) [[unlikely]] {
        Py_DECREF(rest);
        gotoErrorHandler();
    }
    assert(PyTuple_CheckExact(keys));
    for (auto i : IntRange(PyTuple_GET_SIZE(keys))) {
        if (PyDict_DelItem(rest, PyTuple_GET_ITEM(keys, i))) {
            Py_DECREF(rest);
            gotoErrorHandler();
        }
    }
    return rest;
}


PyObject *handle_LOAD_BUILD_CLASS(PyObject *builtins) {
    static _Py_Identifier PyId___build_class__{"__build_class__", -1};
    PyObject *build_class_str = _PyUnicode_FromId(&PyId___build_class__);
    gotoErrorHandler(!build_class_str);

    PyThreadState *tstate;
    bool format_error;
    if (PyDict_CheckExact(builtins)) {
        auto hash = reinterpret_cast<PyASCIIObject *>(build_class_str)->hash;
        auto bc = _PyDict_GetItem_KnownHash(builtins, build_class_str, hash);
        if (bc) {
            Py_INCREF(bc);
            return bc;
        }
        format_error = !_PyErr_Occurred(tstate = _PyThreadState_GET());

    } else {
        auto bc = PyObject_GetItem(builtins, build_class_str);
        if (bc) {
            return bc;
        }
        format_error = _PyErr_ExceptionMatches(tstate = _PyThreadState_GET(), PyExc_KeyError);
    }
    if (format_error) {
        _PyErr_SetString(tstate, PyExc_NameError, "__build_class__ not found");
    }
    gotoErrorHandler(tstate);
}

// TODO: tstate直接传进来么
PyObject *handle_IMPORT_NAME(PyFrameObject *f, PyObject *name, PyObject *fromlist, PyObject *level) {
    auto tstate = _PyThreadState_GET();

    static _Py_Identifier PyId___import__{"__import__", -1};
    PyObject *import_str = _PyUnicode_FromId(&PyId___import__);
    gotoErrorHandler(!import_str, tstate);

    auto hash = reinterpret_cast<PyASCIIObject *>(import_str)->hash;
    auto import_func = _PyDict_GetItem_KnownHash(f->f_builtins, import_str, hash);
    if (!import_func) {
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetString(tstate, PyExc_ImportError, "__import__ not found");
        }
        gotoErrorHandler(tstate);
    }

    if (import_func == tstate->interp->import_func) {
        auto ilevel = _PyLong_AsInt(level);
        gotoErrorHandler(ilevel == -1 && _PyErr_Occurred(tstate), tstate);
        auto res = PyImport_ImportModuleLevelObject(
                name, f->f_globals, f->f_locals ? f->f_locals : Py_None, fromlist, ilevel);
        gotoErrorHandler(!res, tstate);
        return res;
    }

    Py_INCREF(import_func);
    PyObject *stack[]{name, f->f_globals, f->f_locals ? f->f_locals : Py_None, fromlist, level};
    auto res = _PyObject_FastCall(import_func, stack, 5);
    Py_DECREF(import_func);
    gotoErrorHandler(!res, tstate);
    return res;
}

PyObject *handle_IMPORT_FROM(PyObject *from, PyObject *name) {
    PyObject *attr;
    auto err = _PyObject_LookupAttr(from, name, &attr);
    if (err > 0) {
        assert(attr);
        return attr;
    } else {
        gotoErrorHandler(err < 0);
    }

    auto pkgname = _PyObject_GetAttrId(from, &PyId___name__);
    PyObject *pkgname_or_unknown;
    PyThreadState *tstate;
    if (pkgname && PyUnicode_Check(pkgname)) {
        auto fullmodname = PyUnicode_FromFormat("%U.%U", pkgname, name);
        if (!fullmodname) {
            Py_DECREF(pkgname);
            gotoErrorHandler();
        }
        auto mod = PyImport_GetModule(fullmodname);
        Py_DECREF(fullmodname);
        if (mod) {
            Py_DECREF(pkgname);
            return mod;
        }
        tstate = _PyThreadState_GET();
        gotoErrorHandler(_PyErr_Occurred(tstate), tstate);
        pkgname_or_unknown = pkgname;
    } else {
        tstate = _PyThreadState_GET();
        if (pkgname) {
            Py_DECREF(pkgname);
            pkgname = nullptr;
        }
        pkgname_or_unknown = PyUnicode_FromString("<unknown module name>");
        gotoErrorHandler(!pkgname_or_unknown, tstate);
    }

    auto pkgpath = PyModule_GetFilenameObject(from);
    PyObject *errmsg;
    if (pkgpath) {
        static _Py_Identifier PyId___spec__{"__spec__", -1};
        PyObject *spec = _PyObject_GetAttrId(from, &PyId___spec__);
        errmsg = PyUnicode_FromFormat(_PyModuleSpec_IsInitializing(spec) ?
                        "cannot import name %R from partially initialized module %R "
                        "(most likely due to a circular import) (%S)" :
                        "cannot import name %R from %R (%S)",
                name, pkgname_or_unknown, pkgpath);
        Py_XDECREF(spec);
    } else {
        _PyErr_Clear(tstate);
        errmsg = PyUnicode_FromFormat("cannot import name %R from %R (unknown location)",
                name, pkgname_or_unknown);
    }

    PyErr_SetImportError(errmsg, pkgname, pkgpath);
    Py_XDECREF(pkgpath);
    Py_XDECREF(errmsg);
    Py_DECREF(pkgname_or_unknown);
    gotoErrorHandler(tstate);
}

void handle_IMPORT_STAR(PyFrameObject *f, PyObject *from) {
    gotoErrorHandler(PyFrame_FastToLocalsWithError(f) < 0);
    auto locals = f->f_locals;
    if (!locals) {
        auto tstate = _PyThreadState_GET();
        _PyErr_SetString(tstate, PyExc_SystemError, "no locals found during 'import *'");
        gotoErrorHandler(tstate);
    }


    bool skip_leading_underscores = false;
    PyObject *all;
    static _Py_Identifier PyId___all__{"__all__", -1};
    gotoErrorHandler(_PyObject_LookupAttrId(from, &PyId___all__, &all) < 0);
    if (!all) {
        PyObject *dict;
        static _Py_Identifier PyId___dict__{"__dict__", -1};
        if (_PyObject_LookupAttrId(from, &PyId___dict__, &dict) < 0) {
            gotoErrorHandler();
        }
        if (!dict) {
            auto tstate = _PyThreadState_GET();
            _PyErr_SetString(tstate, PyExc_ImportError, "from-import-* object has no __dict__ and no __all__");
            gotoErrorHandler(tstate);
        }
        all = PyMapping_Keys(dict);
        Py_DECREF(dict);
        if (!all) {
            gotoErrorHandler();
        }
        skip_leading_underscores = true;
    }

    PyObject *name;
    bool err = true;
    auto setter = PyDict_CheckExact(locals) ? PyDict_SetItem : PyObject_SetItem;
    for (Py_ssize_t pos = 0;; pos++) {
        name = PySequence_GetItem(all, pos);
        if (!name) {
            auto tstate = _PyThreadState_GET();
            if (_PyErr_ExceptionMatches(tstate, PyExc_IndexError)) {
                err = false;
                _PyErr_Clear(tstate);
            }
            break;
        }
        if (!PyUnicode_Check(name)) {
            PyObject *modname = _PyObject_GetAttrId(from, &PyId___name__);
            if (!modname) {
                break;
            }
            auto tstate = _PyThreadState_GET();
            if (!PyUnicode_Check(modname)) {
                _PyErr_Format(tstate, PyExc_TypeError,
                        "module __name__ must be a string, not %.100s", Py_TYPE(modname)->tp_name);
            } else {
                _PyErr_Format(tstate, PyExc_TypeError,
                        "%s in %U.%s must be str, not %.100s",
                        skip_leading_underscores ? "Key" : "Item",
                        modname,
                        skip_leading_underscores ? "__dict__" : "__all__",
                        Py_TYPE(name)->tp_name);
            }
            Py_DECREF(modname);
            break;
        }
        if (skip_leading_underscores) {
            if (PyUnicode_READY(name) == -1) {
                break;
            }
            if (PyUnicode_READ_CHAR(name, 0) == '_') {
                Py_DECREF(name);
                continue;
            }
        }
        if (auto value = PyObject_GetAttr(from, name)) {
            auto error_while_set = setter(locals, name, value);
            Py_DECREF(value);
            if (error_while_set) {
                break;
            }
            Py_DECREF(name);
        } else {
            break;
        }
    }
    Py_XDECREF(name);
    Py_DECREF(all);
    PyFrame_LocalsToFast(f, 0);
    gotoErrorHandler(err);
}

void handle_POP_EXCEPT(PyFrameObject *f) {
    auto b = PyFrame_BlockPop(f);
    auto tstate = _PyThreadState_GET();
    if (b->b_type != EXCEPT_HANDLER) {
        _PyErr_SetString(tstate, PyExc_SystemError, "popped block is not an except handler");
        gotoErrorHandler(tstate);
    }
    auto stackdepth = getStackDepth(tstate, f);
    assert(stackdepth >= (b)->b_level + 3 && stackdepth <= (b)->b_level + 4);
    auto exc_info = tstate->exc_info;
    auto type = exc_info->exc_type;
    auto value = exc_info->exc_value;
    auto traceback = exc_info->exc_traceback;
    exc_info->exc_type = f->f_valuestack[--stackdepth];
    exc_info->exc_value = f->f_valuestack[--stackdepth];
    exc_info->exc_traceback = f->f_valuestack[--stackdepth];
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
    f->f_stackdepth = stackdepth;
}

bool handle_JUMP_IF_NOT_EXC_MATCH(PyObject *left, PyObject *right) {
    static char CANNOT_CATCH_MSG[]{"catching classes that do not inherit from BaseException is not allowed"};
    if (PyTuple_Check(right)) {
        for (auto i = PyTuple_GET_SIZE(right); i--;) {
            auto exc = PyTuple_GET_ITEM(right, i);
            if (!PyExceptionClass_Check(exc)) {
                auto tstate = _PyThreadState_GET();
                _PyErr_SetString(tstate, PyExc_TypeError, CANNOT_CATCH_MSG);
                gotoErrorHandler(tstate);
            }
        }
    } else {
        if (!PyExceptionClass_Check(right)) {
            auto tstate = _PyThreadState_GET();
            _PyErr_SetString(tstate, PyExc_TypeError, CANNOT_CATCH_MSG);
            gotoErrorHandler(tstate);
        }
    }
    auto res = PyErr_GivenExceptionMatches(left, right);
    gotoErrorHandler(res < 0);
    return res > 0;
}

void handle_RERAISE(PyFrameObject *f, bool restore_lasti) {
    assert(f->f_iblock > 0);
    const auto &try_block = f->f_blockstack[f->f_iblock - 1];
    f->f_lasti = restore_lasti ? try_block.b_handler : f->f_lasti;
    auto tstate = _PyThreadState_GET();
    auto stackdepth = getStackDepth(tstate, f);
    assert(stackdepth == try_block.b_level + 6);
    // TODO: 直接传arr[3]进来如何
    PyObject *exc = f->f_valuestack[--stackdepth];
    PyObject *val = f->f_valuestack[--stackdepth];
    PyObject *tb = f->f_valuestack[--stackdepth];
    f->f_stackdepth = stackdepth;
    assert(PyExceptionClass_Check(exc));
    _PyErr_Restore(tstate, exc, val, tb);
    gotoUnwind(tstate, f);
}

void handle_SETUP_WITH(PyFrameObject *f, PyObject **sp, int handler) {
    // TODO：考虑到load消除可能有问题
    static _Py_Identifier PyId___enter__{"__enter__", -1};
    static _Py_Identifier PyId___exit__{"__exit__", -1};

    auto mgr = *--sp;
    auto enter = _PyObject_LookupSpecial(mgr, &PyId___enter__);
    if (!enter) {
        auto tstate = _PyThreadState_GET();
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetObject(tstate, PyExc_AttributeError, _PyUnicode_FromId(&PyId___enter__));
        }
        gotoErrorHandler(tstate);
    }

    auto exit = _PyObject_LookupSpecial(mgr, &PyId___exit__);
    if (!exit) {
        auto tstate = _PyThreadState_GET();
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetObject(tstate, PyExc_AttributeError, _PyUnicode_FromId(&PyId___exit__));
        }
        Py_DECREF(enter);
        gotoErrorHandler(tstate);
    }
    *sp++ = exit;
    Py_DECREF(mgr);
    auto res = _PyObject_CallNoArg(enter);
    Py_DECREF(enter);
    gotoErrorHandler(!res);
    *sp++ = res;
    PyFrame_BlockSetup(f, SETUP_FINALLY, handler, sp - f->f_valuestack - 1); // TODO: 太黑暗了
}


PyObject *handle_WITH_EXCEPT_START(PyObject *exc, PyObject *val, PyObject *tb, PyObject *exit_func) {
    PyObject *stack[4] = {nullptr, exc, val, tb};
    assert(!Py_IsNone(exc));
    assert(!PyLong_Check(exc));
    auto res = PyObject_Vectorcall(exit_func, stack + 1, 3 | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
    gotoErrorHandler(!res);
    return res;
}

PyObject *handle_YIELD_VALUE(PyObject *val) {
    auto &state = _PyInterpreterState_GET()->async_gen;
    assert(!Py_DEBUG || state.value_numfree != -1);

    PyObject *o;
    if (state.value_numfree) {
        o = reinterpret_cast<PyObject *>(state.value_freelist[--state.value_numfree]);
        assert(Py_IS_TYPE(o, &_PyAsyncGenWrappedValue_Type));
        _Py_NewReference(o);
    } else {
        o = _PyObject_GC_New(&_PyAsyncGenWrappedValue_Type);
        // 要不要DECREF
        gotoErrorHandler(!o);
    }

    struct _PyAsyncGenWrappedValueLayout {
        PyObject_HEAD
        PyObject *agw_val;
    };
    Py_INCREF(val);
    reinterpret_cast<_PyAsyncGenWrappedValueLayout *>(o)->agw_val = val;
    _PyObject_GC_TRACK(o);
    return o;
}

PyObject *handle_GET_YIELD_FROM_ITER(PyObject *iterable, bool is_coroutine) {
    if (PyCoro_CheckExact(iterable)) {
        if (!is_coroutine) {
            auto tstate = _PyThreadState_GET();
            _PyErr_SetString(tstate, PyExc_TypeError,
                    "cannot 'yield from' a coroutine object in a non-coroutine generator");
            gotoErrorHandler(tstate);
        }
    } else if (!PyGen_CheckExact(iterable)) {
        auto iter = PyObject_GetIter(iterable);
        gotoErrorHandler(!iter);
        return iter;
    }
    Py_INCREF(iterable);
    return iterable;
}

static PyObject *getAwaitableIter(PyObject *obj) {
    const auto gen_is_coroutine = [](PyObject *o) {
        if (PyGen_CheckExact(o)) {
            auto code = reinterpret_cast<PyCodeObject *>(reinterpret_cast<PyGenObject *>(o)->gi_code);
            if (code->co_flags & CO_ITERABLE_COROUTINE) {
                return true;
            }
        }
        return !!PyCoro_CheckExact(o);
    };

    if (gen_is_coroutine(obj)) {
        Py_INCREF(obj);
        return obj;
    }

    auto type = Py_TYPE(obj);
    if (!type->tp_as_async || !type->tp_as_async->am_await) [[unlikely]] {
        PyErr_Format(PyExc_TypeError, "object %.100s can't be used in 'await' expression", type->tp_name);
        return nullptr;

    }
    PyObject *res = type->tp_as_async->am_await(obj);
    if (!res) [[unlikely]] {
        return nullptr;
    }
    if (gen_is_coroutine(res)) [[unlikely]] {
        PyErr_SetString(PyExc_TypeError, "__await__() returned a coroutine");
        Py_DECREF(res);
        return nullptr;
    }
    auto res_type = Py_TYPE(res);
    if (!res_type->tp_iternext && res_type->tp_iternext == &_PyObject_NextNotImplemented) [[unlikely]] {
        PyErr_Format(PyExc_TypeError, "__await__() returned non-iterator of type '%.100s'", Py_TYPE(res)->tp_name);
        Py_DECREF(res);
        return nullptr;
    }
    return res;
}

PyObject *handle_GET_AWAITABLE(PyObject *iterable, int error_hint) {
    auto iter = getAwaitableIter(iterable);

    auto type = Py_TYPE(iterable);
    if (!iter) {
        if (error_hint && (!type->tp_as_async || !type->tp_as_async->am_await)) {
            auto tstate = _PyThreadState_GET();
            static const char *const error_messages[]{
                    "'async with' received an object from __aenter__ that does not implement __await__: %.100s",
                    "'async with' received an object from __aexit__ that does not implement __await__: %.100s"
            };
            _PyErr_Format(tstate, PyExc_TypeError, error_messages[error_hint - 1], type->tp_name);
        }
        gotoErrorHandler(!iter);
    }
    /* The original implementation calls _PyGen_yf to check,
     * but _PyCoro_GetAwaitableIter never returns a coroutine object.
     * Therefore, we omit it. */
    return iter;
}

PyObject *handle_GET_AITER(PyObject *obj) {
    PyTypeObject *type = Py_TYPE(obj);
    if (!type->tp_as_async || !type->tp_as_async->am_aiter) [[unlikely]] {
        auto tstate = _PyThreadState_GET();
        _PyErr_Format(tstate, PyExc_TypeError,
                "'async for' requires an object with __aiter__ method, got %.100s",
                type->tp_name);
        gotoErrorHandler(tstate);
    }

    auto iter = type->tp_as_async->am_aiter(obj);
    gotoErrorHandler(!iter);
    auto iter_type = Py_TYPE(iter);

    if (!iter_type->tp_as_async || !iter_type->tp_as_async->am_anext) [[unlikely]] {
        auto tstate = _PyThreadState_GET();
        _PyErr_Format(tstate, PyExc_TypeError,
                "'async for' received an object from __aiter__ that does not implement __anext__: %.100s",
                iter_type->tp_name);
        Py_DECREF(iter);
        gotoErrorHandler(tstate);
    }
    return iter;
}

PyObject *handle_GET_ANEXT(PyObject *aiter) {
    PyTypeObject *type = Py_TYPE(aiter);
    if (PyAsyncGen_CheckExact(aiter)) {
        auto awaitable = type->tp_as_async->am_anext(aiter);
        gotoErrorHandler(!awaitable);
        return awaitable;
    } else {
        if (!type->tp_as_async || !type->tp_as_async->am_anext) [[unlikely]] {
            auto tstate = _PyThreadState_GET();
            _PyErr_Format(tstate, PyExc_TypeError,
                    "'async for' requires an iterator with __anext__ method, got %.100s",
                    type->tp_name);
            gotoErrorHandler(tstate);
        }
        auto next_iter = type->tp_as_async->am_anext(aiter);
        gotoErrorHandler(!next_iter);

        auto awaitable = getAwaitableIter(next_iter);
        if (!awaitable) [[unlikely]] {
            _PyErr_FormatFromCause(PyExc_TypeError, "'async for' received an invalid object from __anext__: %.100s",
                    Py_TYPE(next_iter)->tp_name);
            Py_DECREF(next_iter);
            gotoErrorHandler();
        }
        Py_DECREF(next_iter);
        return awaitable;
    }
}

void handle_END_ASYNC_FOR(PyFrameObject *f) {
    auto tstate = _PyThreadState_GET();
    PyObject *exc = f->f_valuestack[--f->f_stackdepth];
    assert(PyExceptionClass_Check(exc));
    if (PyErr_GivenExceptionMatches(exc, PyExc_StopAsyncIteration)) {
        PyTryBlock *b = PyFrame_BlockPop(f);
        assert(b->b_type == EXCEPT_HANDLER);
        Py_DECREF(exc);

        PyObject *type, *value, *traceback;
        while (f->f_stackdepth > b->b_level + 3) {
            Py_XDECREF(f->f_valuestack[--f->f_stackdepth]);
        }
        auto exc_info = tstate->exc_info;
        type = exc_info->exc_type;
        value = exc_info->exc_value;
        traceback = exc_info->exc_traceback;
        exc_info->exc_type = f->f_valuestack[--f->f_stackdepth];
        exc_info->exc_value = f->f_valuestack[--f->f_stackdepth];
        exc_info->exc_traceback = f->f_valuestack[--f->f_stackdepth];
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
        Py_DECREF(f->f_valuestack[--f->f_stackdepth]);
    } else {
        PyObject *val = f->f_valuestack[--f->f_stackdepth];
        PyObject *tb = f->f_valuestack[--f->f_stackdepth];
        _PyErr_Restore(tstate, exc, val, tb);
        gotoUnwind(tstate, f);
    }
}

void handle_BEFORE_ASYNC_WITH(PyObject **sp) {
    // TODO：考虑到load消除可能有问题
    static _Py_Identifier PyId___aenter__{"__aenter__", -1};
    static _Py_Identifier PyId___aexit__{"__aexit__", -1};

    auto mgr = *--sp;
    auto enter = _PyObject_LookupSpecial(mgr, &PyId___aenter__);
    if (!enter) {
        auto tstate = _PyThreadState_GET();
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetObject(tstate, PyExc_AttributeError, _PyUnicode_FromId(&PyId___aenter__));
        }
        gotoErrorHandler(tstate);
    }

    auto exit = _PyObject_LookupSpecial(mgr, &PyId___aexit__);
    if (!exit) {
        auto tstate = _PyThreadState_GET();
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetObject(tstate, PyExc_AttributeError, _PyUnicode_FromId(&PyId___aexit__));
        }
        Py_DECREF(enter);
        gotoErrorHandler(tstate);
    }
    *sp++ = exit;
    Py_DECREF(mgr);
    auto res = _PyObject_CallNoArg(enter);
    Py_DECREF(enter);
    gotoErrorHandler(!res);
    *sp++ = res;
}

const array<const char *, external_symbol_count> symbol_names{apply(
        [](auto &&... x) noexcept { return array{x.second ...}; },
        external_symbols
)};
const array<void *, external_symbol_count> symbol_addresses{apply(
        [](auto &&... x) noexcept { return array{reinterpret_cast<void *>(x.first) ...}; },
        external_symbols
)};

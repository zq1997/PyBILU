#include <Python.h>
#include <internal/pycore_pyerrors.h>

#include "compile_unit.h"

using namespace std;

static unique_ptr<Translator> translator;
static Py_ssize_t code_extra_index;


PyObject *eval_func(PyThreadState *tstate, PyFrameObject *f, int throwflag) {
    // TODO: manually implement set/get extra
    CompileUnit::TranslatedResult *compiled_result;
    if (_PyCode_GetExtra(reinterpret_cast<PyObject *>(f->f_code),
            code_extra_index,
            reinterpret_cast<void **>(&compiled_result)) == -1) {
        return nullptr;
    }
    if (!compiled_result) {
        return _PyEval_EvalFrameDefault(tstate, f, throwflag);
    }
    // TODO: support generator and throwflag
    assert(!throwflag);


    f->f_state = FRAME_EXECUTING;

    auto prev_cframe = tstate->cframe;
    ExtendedCFrame cframe;
    cframe.sp_map = compiled_result->sp_map.getPointer();
    cframe.use_tracing = prev_cframe->use_tracing;
    cframe.previous = prev_cframe;
    tstate->cframe = &cframe;
    tstate->frame = f;
    PyObject *result;
    if (f->f_lasti < 0) {
        f->f_blockstack[CO_MAXBLOCKS - 1].b_handler = 0;
    }
    auto status = setjmp(cframe.frame_jmp_buf);
    if (status <= 1) [[likely]] {
        result = (*compiled_result)(&symbol_addresses[0], f);
    } else {
        result = nullptr;
    }
    assert(f->f_state == FRAME_SUSPENDED || f->f_stackdepth == 0);
    tstate->cframe = prev_cframe;
    tstate->frame = f->f_back;
    assert(!!result ^ !!_PyErr_Occurred(tstate));
    return result;
}

void freeExtra(void *result) {
    auto result_ = reinterpret_cast< CompileUnit::TranslatedResult *>(result);
    unloadCode(result_->mem_block);
    delete result_;
}

PyObject *apply(PyObject *, PyObject *maybe_func) {
    if (!PyFunction_Check(maybe_func)) {
        PyErr_SetString(PyExc_TypeError, "bad argument type");
        return nullptr;
    }
    auto func = reinterpret_cast<PyFunctionObject *>(maybe_func);
    CompileUnit::TranslatedResult *result;
    try {
        result = CompileUnit::emit(*translator, func->func_code);
    } catch (runtime_error &err) {
        PyErr_SetString(PyExc_RuntimeError, err.what());
        return nullptr;
    } catch (bad_exception &) {
        return nullptr;
    }
    _PyCode_SetExtra(func->func_code, code_extra_index, result);
    return Py_NewRef(func);
}

PyMODINIT_FUNC PyInit_compyler() {
    try {
        translator = make_unique<Translator>();
    } catch (runtime_error &err) {
        PyErr_SetString(PyExc_RuntimeError, err.what());
        return nullptr;
    }

    static PyMethodDef meth_def[] = {{"apply", apply, METH_O}, {}};
    static PyModuleDef mod_def = {
            PyModuleDef_HEAD_INIT,
            "comPyler",
            "A lightweight Just-In-Time compiler for Python",
            -1,
            meth_def
    };
    code_extra_index = _PyEval_RequestCodeExtraIndex(freeExtra);
    if (code_extra_index < 0) {
        PyErr_SetString(PyExc_RuntimeError, "failed to setup");
        return nullptr;
    }
    auto this_mod = PyModule_Create(&mod_def);
    if (this_mod) {
        _PyInterpreterState_SetEvalFrameFunc(PyInterpreterState_Get(), eval_func);
    }
    return PyModule_Create(&mod_def);
}

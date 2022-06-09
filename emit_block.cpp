#include <stdexcept>

#include <Python.h>

#include "compile_unit.h"

using namespace std;
using namespace llvm;

void CompileUnit::emitRotN(PyOparg n) {
    auto &abs_top = abstract_stack[abstract_stack_height - 1];
    unsigned n_lift = 0;
    for (auto i : IntRange(1, n)) {
        auto v = abstract_stack[abstract_stack_height - i] = abstract_stack[abstract_stack_height - (i + 1)];
        n_lift += v.really_pushed;
    }
    abstract_stack[abstract_stack_height - n] = abs_top;

    if (abs_top.really_pushed && n_lift) {
        auto dest_begin = getStackSlot(1);
        auto dest_end = getStackSlot(n);
        auto top = loadValue<PyObject *>(dest_begin, context.tbaa_frame_value);
        if (n_lift <= 16 + 1) {
            auto dest = dest_begin;
            for (auto i : IntRange(1U, n_lift - 1)) {
                auto src = getStackSlot(i + 1);
                auto value = loadValue<PyObject *>(src, context.tbaa_frame_value);
                storeValue<PyObject *>(value, dest, context.tbaa_frame_value);
                dest = src;
            }
        } else {
            auto loop_block = appendBlock("ROT_N.loop");
            auto end_block = appendBlock("ROT_N.end");
            builder.CreateBr(loop_block);
            builder.SetInsertPoint(loop_block);
            auto dest = builder.CreatePHI(context.type<PyObject *>(), 2);
            dest->addIncoming(dest_begin, builder.GetInsertBlock());
            auto src = builder.CreatePHI(context.type<PyObject *>(), 2);
            src->addIncoming(getStackSlot(2), builder.GetInsertBlock());
            auto value = loadValue<PyObject *>(src, context.tbaa_frame_value);
            storeValue<PyObject *>(value, dest, context.tbaa_frame_value);
            auto next_src = getPointer<PyObject *>(src, -1);
            builder.CreateCondBr(builder.CreateICmpEQ(dest, dest_end), end_block, loop_block);
            dest->addIncoming(src, loop_block);
            src->addIncoming(next_src, loop_block);
            builder.SetInsertPoint(end_block);
        }
        storeValue<PyObject *>(top, dest_end, context.tbaa_frame_value);
        return;
    }
}

void CompileUnit::emitBlock(PyBasicBlock &this_block) {
    auto &defined_locals = this_block.locals_input;

    for (auto i : IntRange(1, stack_height + 1)) {
        abstract_stack[abstract_stack_height - i]
                = {loadValue<PyObject *>(getStackSlot(i), context.tbaa_frame_value), true};
    }

    const PyInstrPointer py_instr{py_code};
    PyOparg extended_oparg = 0;

    auto start_index = &this_block == blocks.getPointer() ? 0 : (&this_block)[-1].end_index;
    for (auto vpc : IntRange(start_index, this_block.end_index)) {
        // 注意stack_height记录于此，这就意味着在调用”风险函数“之前不允许DECREF，否则可能DEC两次
        storeValue<decltype(PyFrameObject::f_lasti)>(asValue(vpc), rt_lasti, context.tbaa_frame_value);
        vpc_to_stack_height[vpc] = stack_height;
        di_builder.setLocation(builder, vpc);

        auto opcode = (py_instr + vpc).opcode();
        auto oparg = (py_instr + vpc).rawOparg();
        oparg |= extended_oparg;
        extended_oparg = 0;

        switch (opcode) {
        case EXTENDED_ARG: {
            extended_oparg = oparg << PyInstrPointer::extended_arg_shift;
            break;
        }
        case NOP: {
            break;
        }
        case ROT_TWO: {
            emitRotN(2);
            break;
        }
        case ROT_THREE: {
            emitRotN(3);
            break;
        }
        case ROT_FOUR: {
            emitRotN(4);
            break;
        }
        case ROT_N: {
            emitRotN(oparg);
            break;
        }
        case DUP_TOP: {
            auto top = abstract_stack[abstract_stack_height - 1];
            abstract_stack[abstract_stack_height++] = top;
            if (top.really_pushed) {
                do_Py_INCREF(top.value);
                do_PUSH(top.value);
            }
            break;
        }
        case DUP_TOP_TWO: {
            auto second = abstract_stack[abstract_stack_height - 2];
            auto top = abstract_stack[abstract_stack_height - 1];
            abstract_stack[abstract_stack_height++] = second;
            abstract_stack[abstract_stack_height++] = top;
            if (second.really_pushed) {
                do_Py_INCREF(second.value);
                do_PUSH(second.value);
            }
            if (top.really_pushed) {
                do_Py_INCREF(top.value);
                do_PUSH(top.value);
            }
            break;
        }
        case POP_TOP: {
            auto value = do_POP();
            do_Py_DECREF(value);
            break;
        }
        case LOAD_CONST: {
            // TODO: 添加non null属性
            PyObject *repr{};
            if constexpr (debug_build) {
                repr = PyObject_Repr(PyTuple_GET_ITEM(py_code->co_consts, oparg));
                if (!repr) {
                    throw std::runtime_error("cannot get const object repr");
                }
            }
            auto ptr = getPointer<PyObject *>(code_consts, oparg);
            auto value = loadValue<PyObject *>(ptr, context.tbaa_code_const, useName(repr));
            if constexpr (debug_build) {
                Py_DECREF(repr);
            }
            auto is_redundant = redundant_loads.get(vpc);
            if (!is_redundant) {
                do_Py_INCREF(value);
            }
            do_PUSH(value, !is_redundant);
            break;
        }
        case LOAD_FAST: {
            auto [_, value] = do_GETLOCAL(oparg);
            if (!defined_locals.get(oparg)) {
                auto ok_block = appendBlock("LOAD_FAST.OK");
                // TODO: goto error提取为一个函数来实现
                builder.CreateCondBr(builder.CreateICmpNE(value, context.c_null), ok_block, error_block, context.likely_true);
                builder.SetInsertPoint(ok_block);
            }
            auto is_redundant = redundant_loads.get(vpc);
            if (!is_redundant) {
                do_Py_INCREF(value);
            }
            do_PUSH(value, !is_redundant);
            defined_locals.set(oparg);
            break;
        }
        case STORE_FAST: {
            auto [slot, old_value] = do_GETLOCAL(oparg);
            popAndSave(slot, context.tbaa_frame_value);
            if (!defined_locals.get(oparg)) {
                do_Py_XDECREF(old_value);
            } else {
                do_Py_DECREF(old_value);
            }
            defined_locals.set(oparg);
            break;
        }
        case DELETE_FAST: {
            auto [slot, value] = do_GETLOCAL(oparg);
            if (!defined_locals.get(oparg)) {
                auto ok_block = appendBlock("DELETE_FAST.OK");
                builder.CreateCondBr(builder.CreateICmpNE(value, context.c_null), ok_block, error_block, context.likely_true);
                builder.SetInsertPoint(ok_block);
            }
            storeValue<PyObject *>(context.c_null, slot, context.tbaa_frame_value);
            do_Py_DECREF(value);
            defined_locals.reset(oparg);
            break;
        }
        case LOAD_DEREF: {
            auto cell = getFreevar(oparg);
            auto value = loadFieldValue(cell, &PyCellObject::ob_ref, context.tbaa_obj_field);
            auto ok_block = appendBlock("LOAD_DEREF.OK");
            builder.CreateCondBr(builder.CreateICmpNE(value, context.c_null), ok_block, error_block, context.likely_true);
            builder.SetInsertPoint(ok_block);
            do_Py_INCREF(value);
            do_PUSH(value);
            break;
        }
        case LOAD_CLASSDEREF: {
            auto value = callSymbol<handle_LOAD_CLASSDEREF>(frame_obj, asValue(oparg));
            do_PUSH(value);
            break;
        }
        case STORE_DEREF: {
            auto cell = getFreevar(oparg);
            auto cell_slot = getPointer(cell, &PyCellObject::ob_ref);
            auto old_value = loadValue<PyObject *>(cell_slot, context.tbaa_obj_field);
            popAndSave(cell_slot, context.tbaa_obj_field);
            do_Py_XDECREF(old_value);
            break;
        }
        case DELETE_DEREF: {
            auto cell = getFreevar(oparg);
            auto cell_slot = getPointer(cell, &PyCellObject::ob_ref);
            auto old_value = loadValue<PyObject *>(cell_slot, context.tbaa_obj_field);
            auto ok_block = appendBlock("DELETE_DEREF.OK");
            builder.CreateCondBr(builder.CreateICmpNE(old_value, context.c_null), ok_block, error_block, context.likely_true);
            builder.SetInsertPoint(ok_block);
            storeValue<PyObject *>(context.c_null, cell_slot, context.tbaa_obj_field);
            do_Py_DECREF(old_value);
            break;
        }
        case LOAD_GLOBAL: {
            auto value = callSymbol<handle_LOAD_GLOBAL>(frame_obj, getName(oparg));
            do_PUSH(value);
            break;
        }
        case STORE_GLOBAL: {
            auto value = do_POP();
            callSymbol<handle_STORE_GLOBAL>(frame_obj, getName(oparg), value);
            do_Py_DECREF(value);
            break;
        }
        case DELETE_GLOBAL: {
            callSymbol<handle_DELETE_GLOBAL>(frame_obj, getName(oparg));
            break;
        }
        case LOAD_NAME: {
            auto value = callSymbol<handle_LOAD_NAME>(frame_obj, getName(oparg));
            do_PUSH(value);
            break;
        }
        case STORE_NAME: {
            auto value = do_POP();
            callSymbol<handle_STORE_NAME>(frame_obj, getName(oparg), value);
            do_Py_DECREF(value);
            break;
        }
        case DELETE_NAME: {
            callSymbol<handle_DELETE_NAME>(frame_obj, getName(oparg));
            break;
        }
        case LOAD_ATTR: {
            auto owner = do_POP();
            auto attr = callSymbol<handle_LOAD_ATTR>(owner, getName(oparg));
            do_PUSH(attr);
            do_Py_DECREF(owner);
            break;
        }
        case LOAD_METHOD: {
            callSymbol<handle_LOAD_METHOD>(getName(oparg), getStackSlot(1));
            stack_height += 1;
            abstract_stack_height += 1;
            abstract_stack[abstract_stack_height - 1] = {nullptr, true};
            abstract_stack[abstract_stack_height - 2] = {nullptr, true};
            break;
        }
        case STORE_ATTR: {
            auto owner = do_POP();
            auto value = do_POP();
            callSymbol<handle_STORE_ATTR>(owner, getName(oparg), value);
            do_Py_DECREF(value);
            do_Py_DECREF(owner);
            break;
        }
        case DELETE_ATTR: {
            auto owner = do_POP();
            callSymbol<handle_STORE_ATTR>(owner, getName(oparg), context.c_null);
            do_Py_DECREF(owner);
            break;
        }
        case BINARY_SUBSCR: {
            emit_BINARY_OP<handle_BINARY_SUBSCR>();
            break;
        }
        case STORE_SUBSCR: {
            auto sub = do_POP();
            auto container = do_POP();
            auto value = do_POP();
            callSymbol<handle_STORE_SUBSCR>(container, sub, value);
            do_Py_DECREF(value);
            do_Py_DECREF(container);
            do_Py_DECREF(sub);
            break;
        }
        case DELETE_SUBSCR: {
            auto sub = do_POP();
            auto container = do_POP();
            callSymbol<handle_STORE_SUBSCR>(container, sub, context.c_null);
            do_Py_DECREF(container);
            do_Py_DECREF(sub);
            break;
        }
        case UNARY_NOT: {
            emit_UNARY_OP<handle_UNARY_NOT>();
            break;
        }
        case UNARY_POSITIVE: {
            emit_UNARY_OP<handle_UNARY_POSITIVE>();
            break;
        }
        case UNARY_NEGATIVE: {
            emit_UNARY_OP<handle_UNARY_NEGATIVE>();
            break;
        }
        case UNARY_INVERT: {
            emit_UNARY_OP<handle_UNARY_INVERT>();
            break;
        }
        case BINARY_ADD: {
            emit_BINARY_OP<handle_BINARY_ADD>();
            break;
        }
        case INPLACE_ADD: {
            emit_BINARY_OP<handle_INPLACE_ADD>();
            break;
        }
        case BINARY_SUBTRACT: {
            emit_BINARY_OP<handle_BINARY_SUBTRACT>();
            break;
        }
        case INPLACE_SUBTRACT: {
            emit_BINARY_OP<handle_INPLACE_SUBTRACT>();
            break;
        }
        case BINARY_MULTIPLY: {
            emit_BINARY_OP<handle_BINARY_MULTIPLY>();
            break;
        }
        case INPLACE_MULTIPLY: {
            emit_BINARY_OP<handle_INPLACE_MULTIPLY>();
            break;
        }
        case BINARY_FLOOR_DIVIDE: {
            emit_BINARY_OP<handle_BINARY_FLOOR_DIVIDE>();
            break;
        }
        case INPLACE_FLOOR_DIVIDE: {
            emit_BINARY_OP<handle_INPLACE_FLOOR_DIVIDE>();
            break;
        }
        case BINARY_TRUE_DIVIDE: {
            emit_BINARY_OP<handle_BINARY_TRUE_DIVIDE>();
            break;
        }
        case INPLACE_TRUE_DIVIDE: {
            emit_BINARY_OP<handle_INPLACE_TRUE_DIVIDE>();
            break;
        }
        case BINARY_MODULO: {
            emit_BINARY_OP<handle_BINARY_MODULO>();
            break;
        }
        case INPLACE_MODULO: {
            emit_BINARY_OP<handle_INPLACE_MODULO>();
            break;
        }
        case BINARY_POWER: {
            emit_BINARY_OP<handle_BINARY_POWER>();
            break;
        }
        case INPLACE_POWER: {
            emit_BINARY_OP<handle_INPLACE_POWER>();
            break;
        }
        case BINARY_MATRIX_MULTIPLY: {
            emit_BINARY_OP<handle_BINARY_MATRIX_MULTIPLY>();
            break;
        }
        case INPLACE_MATRIX_MULTIPLY: {
            emit_BINARY_OP<handle_INPLACE_MATRIX_MULTIPLY>();
            break;
        }
        case BINARY_LSHIFT: {
            emit_BINARY_OP<handle_BINARY_LSHIFT>();
            break;
        }
        case INPLACE_LSHIFT: {
            emit_BINARY_OP<handle_INPLACE_LSHIFT>();
            break;
        }
        case BINARY_RSHIFT: {
            emit_BINARY_OP<handle_BINARY_RSHIFT>();
            break;
        }
        case INPLACE_RSHIFT: {
            emit_BINARY_OP<handle_INPLACE_RSHIFT>();
            break;
        }
        case BINARY_AND: {
            emit_BINARY_OP<handle_BINARY_AND>();
            break;
        }
        case INPLACE_AND: {
            emit_BINARY_OP<handle_INPLACE_AND>();
            break;
        }
        case BINARY_OR: {
            emit_BINARY_OP<handle_BINARY_OR>();
            break;
        }
        case INPLACE_OR: {
            emit_BINARY_OP<handle_INPLACE_OR>();
            break;
        }
        case BINARY_XOR: {
            emit_BINARY_OP<handle_BINARY_XOR>();
            break;
        }
        case INPLACE_XOR: {
            emit_BINARY_OP<handle_INPLACE_XOR>();
            break;
        }
        case COMPARE_OP: {
            auto right = do_POP();
            auto left = do_POP();
            // TODO: asValue应该是形参类型
            auto res = callSymbol<handle_COMPARE_OP>(left, right, asValue<int>(oparg));
            do_PUSH(res);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            break;
        }
        case IS_OP: {
            auto right = do_POP();
            auto left = do_POP();
            auto py_true = getSymbol(searchSymbol<_Py_TrueStruct>());
            auto py_false = getSymbol(searchSymbol<_Py_FalseStruct>());
            auto value_for_true = !oparg ? py_true : py_false;
            auto value_for_false = !oparg ? py_false : py_true;
            builder.CreateSelect(builder.CreateICmpEQ(left, right), value_for_true, value_for_false);
            break;
        }
        case CONTAINS_OP: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = callSymbol<handle_CONTAINS_OP>(left, right, asValue<bool>(oparg));
            auto py_true = getSymbol(searchSymbol<_Py_TrueStruct>());
            auto py_false = getSymbol(searchSymbol<_Py_FalseStruct>());
            auto value_for_true = !oparg ? py_true : py_false;
            auto value_for_false = !oparg ? py_false : py_true;
            auto value = builder.CreateSelect(res, value_for_true, value_for_false);
            do_PUSH(value);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            break;
        }
        case RETURN_VALUE: {
            // TODO: 既然是耗尽栈了，应该想办法
            auto retval = do_POP_with_newref();
            assert(stack_height == 0);
            storeFiledValue(asValue<PyFrameState>(FRAME_RETURNED), frame_obj, &PyFrameObject::f_state, context.tbaa_obj_field);
            storeFiledValue(asValue<int>(0), frame_obj, &PyFrameObject::f_stackdepth, context.tbaa_obj_field);
            builder.CreateRet(retval);
            break;
        }
        case CALL_FUNCTION: {
            auto func_args = do_POP_N(oparg + 1);
            auto ret = callSymbol<handle_CALL_FUNCTION>(func_args, asValue<Py_ssize_t>(oparg));
            do_PUSH(ret);
            break;
        }
        case CALL_METHOD: {
            auto func_args = do_POP_N(oparg + 2);
            auto ret = callSymbol<handle_CALL_METHOD>(func_args, asValue<Py_ssize_t>(oparg));
            do_PUSH(ret);
            break;
        }
        case CALL_FUNCTION_KW: {
            auto func_args = do_POP_N(oparg + 2);
            auto ret = callSymbol<handle_CALL_FUNCTION_KW>(func_args, asValue<Py_ssize_t>(oparg));
            do_PUSH(ret);
            break;
        }
        case CALL_FUNCTION_EX: {
            llvm::Value *kwargs = context.c_null;
            if (oparg & 1) {
                kwargs = do_POP();
            }
            auto args = do_POP();
            auto callable = do_POP();
            auto ret = callSymbol<handle_CALL_FUNCTION_EX>(callable, args, kwargs);
            do_PUSH(ret);
            if (oparg & 1) {
                do_Py_DECREF(kwargs);
            }
            do_Py_DECREF(args);
            do_Py_DECREF(callable);
            break;
        }
        case LOAD_CLOSURE: {
            auto cell = loadValue<PyObject *>(getFreevar(oparg), context.tbaa_code_const);
            do_Py_INCREF(cell);
            do_PUSH(cell);
            break;
        }
        case MAKE_FUNCTION: {
            auto qualname = do_POP();
            auto codeobj = do_POP();
            auto globals = loadFieldValue(frame_obj, &PyFrameObject::f_globals, context.tbaa_code_const);
            auto py_func = callSymbol<PyFunction_NewWithQualName>(codeobj, globals, qualname);
            auto ok_block = appendBlock("MAKE_FUNCTION.OK");
            builder.CreateCondBr(builder.CreateICmpNE(py_func, context.c_null), ok_block, error_block, context.likely_true);
            builder.SetInsertPoint(ok_block);
            // TODO: 一个新的tbaa
            if (oparg & 8) {
                popAndSave(getPointer(py_func, &PyFunctionObject::func_closure), context.tbaa_obj_field);
            }
            if (oparg & 4) {
                popAndSave(getPointer(py_func, &PyFunctionObject::func_annotations), context.tbaa_obj_field);
            }
            if (oparg & 2) {
                popAndSave(getPointer(py_func, &PyFunctionObject::func_kwdefaults), context.tbaa_obj_field);
            }
            if (oparg & 1) {
                popAndSave(getPointer(py_func, &PyFunctionObject::func_defaults), context.tbaa_obj_field);
            }
            do_PUSH(py_func);
            do_Py_DECREF(codeobj);
            do_Py_DECREF(qualname);
            break;
        }
        case LOAD_BUILD_CLASS: {
            auto builtins = loadFieldValue(frame_obj, &PyFrameObject::f_builtins, context.tbaa_code_const);
            auto bc = callSymbol<handle_LOAD_BUILD_CLASS>(builtins);
            do_PUSH(bc);
            break;
        }

        case IMPORT_NAME: {
            auto name = getName(oparg);
            auto fromlist = do_POP();
            auto level = do_POP();
            auto res = callSymbol<handle_IMPORT_NAME>(frame_obj, name, fromlist, level);
            do_PUSH(res);
            do_Py_DECREF(level);
            do_Py_DECREF(fromlist);
            break;
        }
        case IMPORT_FROM: {
            auto from = fetchStackValue(1);
            auto res = callSymbol<handle_IMPORT_FROM>(from, getName(oparg));
            do_PUSH(res);
            break;
        }
        case IMPORT_STAR: {
            auto from = do_POP();
            callSymbol<handle_IMPORT_STAR>(frame_obj, from);
            do_Py_DECREF(from);
            break;
        }

        case JUMP_FORWARD: {
            builder.CreateBr(*this_block.branch);
            break;
        }
        case JUMP_ABSOLUTE: {
            builder.CreateBr(*this_block.branch);
            break;
        }
        case POP_JUMP_IF_TRUE: {
            pyJumpIF(this_block, true, true);
            break;
        }
        case POP_JUMP_IF_FALSE: {
            pyJumpIF(this_block, true, false);
            break;
        }
        case JUMP_IF_TRUE_OR_POP: {
            pyJumpIF(this_block, true, true);
            break;
        }
        case JUMP_IF_FALSE_OR_POP: {
            pyJumpIF(this_block, true, false);
            break;
        }
        case GET_ITER: {
            auto iterable = do_POP();
            auto iter = callSymbol<handle_GET_ITER>(iterable);
            do_Py_DECREF(iterable);
            do_PUSH(iter);
            break;
        }
        case FOR_ITER: {
            // TODO: 不止那么简单还有异常情况
            auto iter = fetchStackValue(1);
            auto the_type = loadFieldValue(iter, &PyObject::ob_type, context.tbaa_obj_field);
            auto the_iternextfunc = loadFieldValue(the_type, &PyTypeObject::tp_iternext, context.tbaa_obj_field);
            auto next = callFunction(context.type<remove_pointer_t<iternextfunc>>(), the_iternextfunc, iter);
            do_PUSH(next);
            auto b_break = appendBlock("FOR_ITER.break");
            builder.CreateCondBr(builder.CreateICmpEQ(next, context.c_null), b_break, this_block.next());
            // iteration should break
            builder.SetInsertPoint(b_break);
            callSymbol<handle_FOR_ITER>();
            do_Py_DECREF(iter.value);
            builder.CreateBr(*this_block.branch);
            break;
        }

        case BUILD_STRING: {
            auto values = do_POP_N(oparg);
            auto str = callSymbol<handle_BUILD_STRING>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(str);
            break;
        }
        case BUILD_TUPLE: {
            auto values = do_POP_N(oparg);
            auto map = callSymbol<handle_BUILD_TUPLE>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_LIST: {
            auto values = do_POP_N(oparg);
            auto map = callSymbol<handle_BUILD_LIST>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_SET: {
            auto values = do_POP_N(oparg);
            auto map = callSymbol<handle_BUILD_SET>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_MAP: {
            auto values = do_POP_N(2 * oparg);
            auto map = callSymbol<handle_BUILD_MAP>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_CONST_KEY_MAP: {
            auto values = do_POP_N(oparg + 1);
            auto map = callSymbol<handle_BUILD_CONST_KEY_MAP>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(map);
            break;
        }
        case LIST_APPEND: {
            auto value = do_POP();
            auto list = fetchStackValue(oparg);
            callSymbol<handle_LIST_APPEND>(list, value);
            do_Py_DECREF(value);
            break;
        }
        case SET_ADD: {
            auto value = do_POP();
            auto set = fetchStackValue(oparg);
            callSymbol<handle_SET_ADD>(set, value);
            do_Py_DECREF(value);
            break;
        }
        case MAP_ADD: {
            auto value = do_POP();
            auto key = do_POP();
            auto map = fetchStackValue(oparg);
            callSymbol<handle_MAP_ADD>(map, key, value);
            do_Py_DECREF(key);
            do_Py_DECREF(value);
            break;
        }
        case LIST_EXTEND: {
            auto iterable = do_POP();
            auto list = fetchStackValue(oparg);
            callSymbol<handle_LIST_EXTEND>(list, iterable);
            do_Py_DECREF(iterable);
            break;
        }
        case SET_UPDATE: {
            auto iterable = do_POP();
            auto set = fetchStackValue(oparg);
            callSymbol<handle_SET_UPDATE>(set, iterable);
            do_Py_DECREF(iterable);
            break;
        }
        case DICT_UPDATE: {
            auto update = do_POP();
            auto dict = fetchStackValue(oparg);
            callSymbol<handle_DICT_UPDATE>(dict, update);
            do_Py_DECREF(update);
            break;
        }
        case DICT_MERGE: {
            auto update = do_POP();
            auto dict = fetchStackValue(oparg);
            auto callee = fetchStackValue(oparg + 2);
            callSymbol<handle_DICT_MERGE>(callee, dict, update);
            do_Py_DECREF(update);
            break;
        }
        case LIST_TO_TUPLE: {
            auto list = do_POP();
            auto tuple = callSymbol<handle_LIST_TO_TUPLE>(list);
            do_PUSH(tuple);
            do_Py_DECREF(list);
            break;
        }

        case FORMAT_VALUE: {
            auto fmt_spec = (oparg & FVS_MASK) == FVS_HAVE_SPEC ? do_POP() : context.c_null;
            auto value = do_POP();
            int which_conversion = oparg & FVC_MASK;
            auto result = callSymbol<handle_FORMAT_VALUE>(value, fmt_spec, asValue(which_conversion));
            do_PUSH(result);
            do_Py_DECREF(value);
            break;
        }

        case UNPACK_SEQUENCE: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case UNPACK_EX: {
            throw runtime_error("unimplemented opcode");
            break;
        }

        case GET_LEN: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case MATCH_MAPPING: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case MATCH_SEQUENCE: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case MATCH_KEYS: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case MATCH_CLASS: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case COPY_DICT_WITHOUT_KEYS: {
            throw runtime_error("unimplemented opcode");
            break;
        }

        case BUILD_SLICE: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case LOAD_ASSERTION_ERROR: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case RAISE_VARARGS: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case SETUP_ANNOTATIONS: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case PRINT_EXPR: {
            throw runtime_error("unimplemented opcode");
            break;
        }

        case SETUP_FINALLY: {
            entry_jump->addDestination(*this_block.branch);
            auto block_addr_diff = builder.CreateIntCast(builder.CreateSub(
                    builder.CreatePtrToInt(BlockAddress::get(function, *this_block.branch), context.type<uintptr_t>()),
                    builder.CreatePtrToInt(BlockAddress::get(function, blocks[0]), context.type<uintptr_t>())
            ), context.type<int>(), true);
            callSymbol<PyFrame_BlockSetup>(frame_obj, asValue<int>(SETUP_FINALLY), block_addr_diff, asValue<int>(stack_height));
            builder.CreateBr(this_block.next());
            break;
        }
        case POP_BLOCK: {
            callSymbol<PyFrame_BlockPop>(frame_obj);
            break;
        }
        case POP_EXCEPT: {
            callSymbol<handle_POP_EXCEPT>(frame_obj);
            break;
        }
        case JUMP_IF_NOT_EXC_MATCH: {
            auto right = do_POP();
            auto left = do_POP();
            auto match = callSymbol<handle_JUMP_IF_NOT_EXC_MATCH>(left, right);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            builder.CreateCondBr(match, this_block.next(), *this_block.branch);
            break;
        }
        case RERAISE: {
            callSymbol<handle_RERAISE, &Context::attr_noreturn>(frame_obj, asValue<bool>(oparg));
            builder.CreateUnreachable();
            break;
        }
        case SETUP_WITH: {
            entry_jump->addDestination(*this_block.branch);
            auto block_addr_diff = builder.CreateIntCast(builder.CreateSub(
                    builder.CreatePtrToInt(BlockAddress::get(function, *this_block.branch), context.type<uintptr_t>()),
                    builder.CreatePtrToInt(BlockAddress::get(function, blocks[0]), context.type<uintptr_t>())
            ), context.type<int>(), true);
            callSymbol<handle_SETUP_WITH>(frame_obj, getStackSlot(), block_addr_diff);
            stack_height += 1;
            builder.CreateBr(this_block.next());
            break;
        }
        case WITH_EXCEPT_START: {
            auto exc = fetchStackValue(1);
            auto val = fetchStackValue(2);
            auto tb = fetchStackValue(3);
            auto exit_func = fetchStackValue(7);
            auto res = callSymbol<handle_WITH_EXCEPT_START>(exc, val, tb, exit_func);
            do_PUSH(res);
            break;
        }

        case GEN_START: {
            auto should_be_none = do_POP();
            do_Py_DECREF(should_be_none);
            auto py_none = getSymbol(searchSymbol<_Py_NoneStruct>());
            auto ok_block = appendBlock("GEN_START.OK");
            builder.CreateCondBr(builder.CreateICmpEQ(should_be_none, py_none), ok_block, error_block, context.likely_true);
            builder.SetInsertPoint(ok_block);
            break;
        }
        case YIELD_VALUE: {
            Value *retval;
            if (py_code->co_flags & CO_ASYNC_GENERATOR) {
                auto poped = do_POP();
                retval = callSymbol<handle_YIELD_VALUE>(retval);
                do_Py_DECREF(poped);
            } else {
                retval = do_POP_with_newref();
            }
            auto resume_block = appendBlock("YIELD_VALUE.resume");
            entry_jump->addDestination(resume_block);
            auto block_addr_diff = builder.CreateIntCast(builder.CreateSub(
                    builder.CreatePtrToInt(BlockAddress::get(function, resume_block), context.type<uintptr_t>()),
                    builder.CreatePtrToInt(BlockAddress::get(function, blocks[0]), context.type<uintptr_t>())
            ), context.type<int>(), true);
            storeValue<int>(block_addr_diff, coroutine_handler, context.tbaa_frame_value);
            storeFiledValue(asValue<PyFrameState>(FRAME_SUSPENDED), frame_obj, &PyFrameObject::f_state, context.tbaa_obj_field);
            storeFiledValue(asValue<int>(stack_height), frame_obj, &PyFrameObject::f_stackdepth, context.tbaa_obj_field);
            builder.CreateRet(retval);
            builder.SetInsertPoint(resume_block);
            stack_height++;
            abstract_stack[abstract_stack_height++] =
                    {loadValue<PyObject *>(getStackSlot(1), context.tbaa_frame_value), true};
            break;
        }
        case GET_YIELD_FROM_ITER: {
            auto iterable = do_POP();
            bool is_coroutine = py_code->co_flags & (CO_COROUTINE | CO_ITERABLE_COROUTINE);
            auto iter = callSymbol<handle_GET_YIELD_FROM_ITER>(iterable, asValue(is_coroutine));
            do_PUSH(iter);
            do_Py_DECREF(iterable);
            break;
        }
        case YIELD_FROM: {
            auto resume_block = appendBlock("YIELD_FROM.resume");
            builder.CreateBr(resume_block);
            builder.SetInsertPoint(resume_block);
            entry_jump->addDestination(resume_block);

            --abstract_stack_height;
            --stack_height;
            assert(abstract_stack[abstract_stack_height].really_pushed);
            auto v = loadValue<PyObject *>(getStackSlot(), context.tbaa_frame_value, "YIELD_FROM.sending_value");
            --abstract_stack_height;
            --stack_height;
            assert(abstract_stack[abstract_stack_height].really_pushed);
            auto receiver = loadValue<PyObject *>(getStackSlot(), context.tbaa_frame_value, "YIELD_FROM.receiver");

            // TODO: trace support when tstate->c_tracefunc != NULL
            auto retval_ptr = builder.CreateAlloca(context.type<PyObject *>());
            auto gen_status = callSymbol<PyIter_Send>(receiver, v, retval_ptr);
            // TODO: ok_block叫b_ok吧，或者反过来，统一化
            auto ok_block = appendBlock("YIELD_FROM.ok");
            builder.CreateCondBr(builder.CreateICmpNE(gen_status, asValue(PYGEN_ERROR)), ok_block, error_block, context.likely_true);
            builder.SetInsertPoint(ok_block);
            auto retval = loadValue<PyObject *>(retval_ptr, nullptr, "retval");
            do_Py_DECREF(v);
            auto b_next = appendBlock("YIELD_FROM.next");
            auto b_return = appendBlock("YIELD_FROM.return");
            builder.CreateCondBr(builder.CreateICmpEQ(gen_status, asValue(PYGEN_NEXT)), b_next, b_return, context.likely_true);

            builder.SetInsertPoint(b_next);
            auto block_addr_diff = builder.CreateIntCast(builder.CreateSub(
                    builder.CreatePtrToInt(BlockAddress::get(function, resume_block), context.type<uintptr_t>()),
                    builder.CreatePtrToInt(BlockAddress::get(function, blocks[0]), context.type<uintptr_t>())
            ), context.type<int>(), true);
            storeValue<int>(block_addr_diff, coroutine_handler, context.tbaa_frame_value);
            storeFiledValue(asValue<PyFrameState>(FRAME_SUSPENDED), frame_obj, &PyFrameObject::f_state, context.tbaa_obj_field);
            storeFiledValue(asValue<int>(stack_height + 1), frame_obj, &PyFrameObject::f_stackdepth, context.tbaa_obj_field);
            builder.CreateRet(retval);

            // TODO: push/pop现在的实现，对处理多分支等情况不太友好，想办法改善
            builder.SetInsertPoint(b_return);
            do_Py_DECREF(receiver);
            do_PUSH(retval);
            break;
        }
        case GET_AWAITABLE: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case GET_AITER: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case GET_ANEXT: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case END_ASYNC_FOR: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case SETUP_ASYNC_WITH: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case BEFORE_ASYNC_WITH: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        default:
            throw runtime_error("unexpected opcode");
        }
    }
    // 排除分支已经跳转
    if (this_block.fall_through && !this_block.branch) {
        assert(!builder.GetInsertBlock()->getTerminator());
        builder.CreateBr(this_block.next());
    }
}

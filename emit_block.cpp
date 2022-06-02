#include <stdexcept>

#include <Python.h>

#include "compile_unit.h"

using namespace std;
using namespace llvm;

void CompileUnit::emitRotN(PyOparg n) {
    auto [top_value, top_really_pushed] = abstract_stack[abstract_stack_height - 1];
    unsigned n_lift = 0;
    for (auto i : IntRange(1, n)) {
        auto [_, really_pushed] = abstract_stack[abstract_stack_height - i]
                = abstract_stack[abstract_stack_height - (i + 1)];
        n_lift += really_pushed;
    }
    abstract_stack[abstract_stack_height - n] = {top_value, top_really_pushed};

    if (top_really_pushed && n_lift) {
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


void CompileUnit::emitBlock(unsigned index) {
    auto &this_block = blocks[index];
    this_block.block->insertInto(function);
    builder.SetInsertPoint(this_block.block);

    auto &defined_locals = this_block.locals_input;

    for (auto i : IntRange(1, stack_height + 1)) {
        abstract_stack[abstract_stack_height - i]
                = {loadValue<PyObject *>(getStackSlot(i), context.tbaa_frame_value), true};
    }

    bool fall_through = true;

    const PyInstrPointer py_instr{py_code};
    PyOparg extended_oparg = 0;

    for (auto vpc = this_block.start; vpc != blocks[index + 1].start; vpc++) {
        // 注意stack_height记录于此，这就意味着在调用”风险函数“之前不允许DECREF，否则可能DEC两次
        storeValue<decltype(PyFrameObject::f_lasti)>(asValue(vpc), rt_lasti, context.tbaa_frame_value);
        vpc_to_stack_height[vpc] = stack_height;
        di_builder.setLocation(builder, vpc);

        auto [opcode, oparg] = py_instr[vpc];
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
            auto [top, top_really_pushed] = abstract_stack[abstract_stack_height - 1];
            abstract_stack[abstract_stack_height++] = {top, top_really_pushed};
            if (top_really_pushed) {
                do_Py_INCREF(top);
                do_PUSH(top);
            }
            break;
        }
        case DUP_TOP_TWO: {
            auto [second, second_really_pushed] = abstract_stack[abstract_stack_height - 2];
            auto [top, top_really_pushed] = abstract_stack[abstract_stack_height - 1];
            abstract_stack[abstract_stack_height++] = {second, second_really_pushed};
            abstract_stack[abstract_stack_height++] = {top, top_really_pushed};
            if (second_really_pushed) {
                do_Py_INCREF(second);
                do_PUSH(second);
            }
            if (top_really_pushed) {
                do_Py_INCREF(top);
                do_PUSH(top);
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
            auto value = do_GETLOCAL(oparg);
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
            auto value = do_POPWithStolenRef();
            do_SETLOCAL(oparg, value, defined_locals.get(oparg));
            defined_locals.set(oparg);
            break;
        }
        case DELETE_FAST: {
            auto value = do_GETLOCAL(oparg);
            auto ok_block = appendBlock("DELETE_FAST.OK");
            builder.CreateCondBr(builder.CreateICmpNE(value, context.c_null), ok_block, error_block, context.likely_true);
            builder.SetInsertPoint(ok_block);
            do_SETLOCAL(oparg, context.c_null, true);
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
            auto value = do_POPWithStolenRef();
            storeValue<PyObject *>(value, cell_slot, context.tbaa_obj_field);
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
            auto obj = do_POP();
            callSymbol<handle_LOAD_METHOD>(obj, getName(oparg), getStackSlot());
            stack_height += 2;
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
            auto retval = fetchStackValue(1);
            do_Py_INCREF(retval);
            builder.CreateRet(retval);
            fall_through = false;
            break;
        }
        case CALL_FUNCTION: {
            // func_args重命名
            stack_height -= oparg + 1;
            auto func_args = getStackSlot();
            auto ret = callSymbol<handle_CALL_FUNCTION>(func_args, asValue<Py_ssize_t>(oparg));
            do_PUSH(ret);
            break;
        }
        case CALL_FUNCTION_KW: {
            stack_height -= oparg + 2;
            auto func_args = getStackSlot();
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
        case CALL_METHOD: {
            auto maybe_meth = fetchStackValue(oparg + 2);
            stack_height -= oparg + 2;
            auto is_meth = builder.CreateICmpNE(maybe_meth, context.c_null);
            auto func_args = builder.CreateSelect(is_meth, getStackSlot(), getStackSlot(-1));
            auto nargs = builder.CreateSelect(is_meth,
                    asValue<Py_ssize_t>(oparg + 1), asValue<Py_ssize_t>(oparg));
            auto ret = callSymbol<handle_CALL_FUNCTION>(func_args, nargs);
            do_PUSH(ret);
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
                storeFiledValue(do_POPWithStolenRef(), py_func, &PyFunctionObject::func_closure, context.tbaa_refcnt);
            }
            if (oparg & 4) {
                storeFiledValue(do_POPWithStolenRef(), py_func, &PyFunctionObject::func_annotations, context.tbaa_refcnt);
            }
            if (oparg & 2) {
                storeFiledValue(do_POPWithStolenRef(), py_func, &PyFunctionObject::func_kwdefaults, context.tbaa_refcnt);
            }
            if (oparg & 1) {
                storeFiledValue(do_POPWithStolenRef(), py_func, &PyFunctionObject::func_defaults, context.tbaa_refcnt);
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
            auto &jmp_block = findPyBlock(vpc + 1 + oparg, stack_height);
            builder.CreateBr(jmp_block.block);
            fall_through = false;
            break;
        }
        case JUMP_ABSOLUTE: {
            // TODO: 如果有，则不要设置，要校验
            auto &jmp_block = findPyBlock(oparg, stack_height);
            builder.CreateBr(jmp_block.block);
            fall_through = false;
            break;
        }
        case POP_JUMP_IF_TRUE: {
            pyJumpIF(oparg, true, true);
            break;
        }
        case POP_JUMP_IF_FALSE: {
            pyJumpIF(oparg, true, false);
            break;
        }
        case JUMP_IF_TRUE_OR_POP: {
            pyJumpIF(oparg, false, true);
            break;
        }
        case JUMP_IF_FALSE_OR_POP: {
            pyJumpIF(oparg, false, false);
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
            auto b_continue = appendBlock("FOR_ITER.continue");
            auto b_break = appendBlock("FOR_ITER.break");
            builder.CreateCondBr(builder.CreateICmpEQ(next, context.c_null), b_break, b_continue);
            // iteration should break
            builder.SetInsertPoint(b_break);
            auto &break_target = findPyBlock(vpc + 1 + oparg, stack_height - 1);
            do_Py_DECREF(iter);
            builder.CreateBr(break_target.block);
            // iteration should continue
            builder.SetInsertPoint(b_continue);
            do_PUSH(next);
            break;
        }

        case BUILD_STRING: {
            stack_height -= oparg;
            auto values = getStackSlot();
            auto str = callSymbol<handle_BUILD_STRING>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(str);
            break;
        }
        case BUILD_TUPLE: {
            stack_height -= oparg;
            auto values = getStackSlot();
            auto map = callSymbol<handle_BUILD_TUPLE>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_LIST: {
            stack_height -= oparg;
            auto values = getStackSlot();
            auto map = callSymbol<handle_BUILD_LIST>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_SET: {
            stack_height -= oparg;
            auto values = getStackSlot();
            auto map = callSymbol<handle_BUILD_SET>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_MAP: {
            stack_height -= 2 * oparg;
            auto values = getStackSlot();
            auto map = callSymbol<handle_BUILD_MAP>(values, asValue<Py_ssize_t>(oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_CONST_KEY_MAP: {
            stack_height -= oparg + 1;
            auto values = getStackSlot();
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
            auto &py_finally_block = findPyBlock(vpc + 1 + oparg, stack_height + 6);
            py_finally_block.is_handler = true;
            auto block_addr_diff = builder.CreateSub(
                    builder.CreatePtrToInt(BlockAddress::get(function, py_finally_block.block), context.type<uintptr_t>()),
                    builder.CreatePtrToInt(BlockAddress::get(function, blocks[1].block), context.type<uintptr_t>())
            );
            callSymbol<PyFrame_BlockSetup>(frame_obj,
                    asValue<int>(SETUP_FINALLY),
                    builder.CreateIntCast(block_addr_diff, context.type<int>(), true),
                    asValue<int>(stack_height));
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
            auto &jump_target = findPyBlock(oparg, stack_height);
            builder.CreateCondBr(match, blocks[index + 1].block, jump_target.block);
            break;
        }
        case RERAISE: {
            callSymbol<handle_RERAISE, &Context::attr_noreturn>(frame_obj, asValue<bool>(oparg));
            builder.CreateUnreachable();
            fall_through = false;
            break;
        }
        case SETUP_WITH: {
            auto &py_finally_block = findPyBlock(vpc + 1 + oparg, stack_height - 1 + 7);
            py_finally_block.is_handler = true;
            auto block_addr_diff = builder.CreateSub(
                    builder.CreatePtrToInt(BlockAddress::get(function, py_finally_block.block), context.type<uintptr_t>()),
                    builder.CreatePtrToInt(BlockAddress::get(function, blocks[1].block), context.type<uintptr_t>())
            );
            callSymbol<handle_SETUP_WITH>(frame_obj, getStackSlot(),
                    builder.CreateIntCast(block_addr_diff, context.type<int>(), true));
            stack_height += 1;
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
            throw runtime_error("unimplemented opcode");
            break;
        }
        case YIELD_VALUE: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case GET_YIELD_FROM_ITER: {
            throw runtime_error("unimplemented opcode");
            break;
        }
        case YIELD_FROM: {
            throw runtime_error("unimplemented opcode");
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
    if (fall_through) {
        assert(!builder.GetInsertBlock()->getTerminator());
        builder.CreateBr(blocks[index + 1].block);
        if (!blocks[index + 1].visited) {
            blocks[index + 1].visited = true;
            pending_block_indices.push(index + 1);
            blocks[index + 1].initial_stack_height = stack_height;
        }
    }
}

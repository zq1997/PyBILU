#include <iostream>

#include "compile_unit.h"

using namespace std;

template <typename T>
class ReversedStack {
    ssize_t size;
    T *const stack;
    T *sp;
    T until_now;

    static constexpr auto until_forever = numeric_limits<T>::max();

public:
    explicit ReversedStack(ssize_t size) : size{size}, stack{new T[size * 2]}, sp{stack + size} {}

    ~ReversedStack() noexcept { delete[] stack; }

    void setTimestamp(T t) { until_now = t; }

    void reset() {
        sp = stack + size;
        for (auto i : IntRange(size)) {
            stack[i] = until_forever;
        }
    }


    void peak(int i, T timestamp) {
        sp[-i] = timestamp;
    }

    T set_peak(int i) {
        return sp[-i];
    }

    void pop(T timestamp) {
        *sp++ = timestamp;
    }

    void pop() {
        pop(until_now);
    }

    void pop_n_consecutively(int n, T timestamp = until_forever) {
        while (n--) {
            pop(timestamp);
        }
    }

    T push() {
        return *--sp;
    }
};


// TODO： 或许这里可以顺便分析出delta_stack_height，后面可以直接利用
void CompileUnit::analyzeRedundantLoads() {
    ReversedStack<unsigned> stack(py_code->co_stacksize);

    const PyInstrPointer py_instr{py_code};
    unsigned code_instr_num = PyBytes_GET_SIZE(py_code->co_code) / sizeof(_Py_CODEUNIT);
    const auto until_finally = code_instr_num;
    constexpr decltype(until_finally) until_anytime = 0;

    redundant_loads.reserve(code_instr_num);
    DynamicArray<unsigned> locals(py_code->co_nlocals);
    for (auto i : IntRange(py_code->co_nlocals)) {
        locals[i] = until_finally;
    }

    assert(block_num >= 1);
    auto block = &blocks[block_num];
    for (auto index = code_instr_num;;) {
        if (index == block->start) {
            if (block != &blocks[block_num]) {
                block->locals_deleted.flipAll(py_code->co_nlocals);
                BitArrayRefWithSize(block->locals_deleted, py_code->co_nlocals) &= block->locals_touched;
                block->locals_touched.flipAll(py_code->co_nlocals);
            }
            if (!index) {
                break;
            }
            --block;
            block->locals_touched.reserve(py_code->co_nlocals);
            block->locals_deleted.reserve(py_code->co_nlocals);
            block->locals_ever_deleted.reserve(py_code->co_nlocals);
            stack.reset();
        }
        auto &locals_touched = block->locals_touched;
        auto &locals_deleted = block->locals_deleted;
        auto &locals_ever_deleted = block->locals_ever_deleted;

        stack.setTimestamp(--index);
        auto instr = py_instr + index;

        switch (instr.opcode()) {
        case EXTENDED_ARG:
        case NOP:
            break;
        case ROT_TWO: {
            auto top = stack.set_peak(2);
            stack.peak(2, stack.set_peak(1));
            stack.peak(1, top);
            break;
        }
        case ROT_THREE: {
            auto top = stack.set_peak(3);
            stack.peak(3, stack.set_peak(2));
            stack.peak(2, stack.set_peak(1));
            stack.peak(1, top);
            break;
        }
        case ROT_FOUR: {
            auto top = stack.set_peak(4);
            stack.peak(4, stack.set_peak(3));
            stack.peak(3, stack.set_peak(2));
            stack.peak(2, stack.set_peak(1));
            stack.peak(1, top);
            break;
        }
        case ROT_N: {
            auto n = instr.fullOparg(py_instr);
            auto top = stack.set_peak(n);
            while (--n) {
                stack.peak(n + 1, stack.set_peak(n));
            }
            stack.peak(1, top);
            break;
        }
        case DUP_TOP: {
            auto t1 = stack.push();
            auto t2 = stack.push();
            stack.pop(max(t1, t2));
            break;
        }
        case DUP_TOP_TWO: {
            auto t1 = stack.push();
            auto t2 = stack.push();
            auto t3 = stack.push();
            auto t4 = stack.push();
            stack.pop(max(t2, t4));
            stack.pop(max(t1, t3));
            break;
        }
        case POP_TOP:
            stack.pop(until_anytime);
            break;
        case LOAD_CONST:
            redundant_loads.setIf(index, until_finally > stack.push());
            break;
        case LOAD_FAST: {
            auto oparg = instr.fullOparg(py_instr);
            redundant_loads.setIf(index, locals[oparg] > stack.push());
            locals_touched.set(oparg);
            break;
        }
        case STORE_FAST: {
            auto oparg = instr.fullOparg(py_instr);
            locals[oparg] = index;
            stack.pop();
            locals_touched.set(oparg);
            break;
        }
        case DELETE_FAST: {
            auto oparg = instr.fullOparg(py_instr);
            locals_deleted.setIf(oparg, !locals_touched.get(oparg));
            locals_ever_deleted.set(oparg);
            locals_touched.set(oparg);
            break;
        }
        case LOAD_DEREF:
        case LOAD_CLASSDEREF:
            stack.push();
            break;
        case STORE_DEREF:
            stack.pop();
            break;
        case DELETE_DEREF:
            break;
        case LOAD_GLOBAL:
            stack.push();
            break;
        case STORE_GLOBAL:
            stack.pop();
            break;
        case DELETE_GLOBAL:
            break;
        case LOAD_NAME:
            stack.push();
            break;
        case STORE_NAME:
            stack.pop();
            break;
        case DELETE_NAME:
            break;
        case LOAD_ATTR:
            stack.push();
            stack.pop();
            break;
        case LOAD_METHOD:
            stack.push();
            stack.push();
            stack.pop();
            break;
        case STORE_ATTR:
            stack.pop();
            stack.pop();
            break;
        case DELETE_ATTR:
            stack.pop();
            break;
        case BINARY_SUBSCR:
            stack.push();
            stack.pop();
            stack.pop();
            break;
        case STORE_SUBSCR:
            stack.pop();
            stack.pop();
            stack.pop();
            break;
        case DELETE_SUBSCR:
            stack.pop();
            stack.pop();
            break;
        case UNARY_NOT:
        case UNARY_POSITIVE:
        case UNARY_NEGATIVE:
        case UNARY_INVERT:
            stack.push();
            stack.pop();
            break;
        case BINARY_ADD:
        case INPLACE_ADD:
        case BINARY_SUBTRACT:
        case INPLACE_SUBTRACT:
        case BINARY_MULTIPLY:
        case INPLACE_MULTIPLY:
        case BINARY_FLOOR_DIVIDE:
        case INPLACE_FLOOR_DIVIDE:
        case BINARY_TRUE_DIVIDE:
        case INPLACE_TRUE_DIVIDE:
        case BINARY_MODULO:
        case INPLACE_MODULO:
        case BINARY_POWER:
        case INPLACE_POWER:
        case BINARY_MATRIX_MULTIPLY:
        case INPLACE_MATRIX_MULTIPLY:
        case BINARY_LSHIFT:
        case INPLACE_LSHIFT:
        case BINARY_RSHIFT:
        case INPLACE_RSHIFT:
        case BINARY_AND:
        case INPLACE_AND:
        case BINARY_OR:
        case INPLACE_OR:
        case BINARY_XOR:
        case INPLACE_XOR:
        case COMPARE_OP:
        case IS_OP:
        case CONTAINS_OP:
            stack.push();
            stack.pop();
            stack.pop();
            break;
        case RETURN_VALUE:
            stack.pop();
            break;

        case CALL_FUNCTION:
            stack.push();
            stack.pop_n_consecutively(1 + instr.fullOparg(py_instr));
            break;
        case CALL_FUNCTION_KW:
            stack.push();
            stack.pop_n_consecutively(2 + instr.fullOparg(py_instr));
            break;
        case CALL_FUNCTION_EX:
            stack.push();
            stack.pop();
            stack.pop();
            if (instr.fullOparg(py_instr) & 1) {
                stack.pop();
            }
            break;
        case CALL_METHOD:
            stack.push();
            stack.pop_n_consecutively(2 + instr.fullOparg(py_instr));
            break;
        case LOAD_CLOSURE:
            stack.push();
            break;
        case MAKE_FUNCTION: {
            stack.push();
            auto oparg = instr.fullOparg(py_instr);
            stack.pop_n_consecutively(2 + !!(oparg & 1) + !!(oparg & 2) + !!(oparg & 4) + !!(oparg & 8));
            break;
        }
        case LOAD_BUILD_CLASS:
            stack.push();
            break;

        case IMPORT_NAME:
            stack.push();
            stack.pop();
            stack.pop();
            break;
        case IMPORT_FROM:
            stack.push();
            break;
        case IMPORT_STAR:
            stack.pop();
            break;

        case JUMP_FORWARD:
        case JUMP_ABSOLUTE:
            break;
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_FALSE:
            stack.pop();
            break;
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
            break;
        case GET_ITER:
            stack.push();
            stack.pop();
            break;
        case FOR_ITER:
            stack.push();
            break;

        case BUILD_TUPLE:
        case BUILD_LIST:
        case BUILD_SET:
            stack.push();
            stack.pop_n_consecutively(instr.fullOparg(py_instr));
            break;
        case BUILD_MAP:
            stack.push();
            stack.pop_n_consecutively(2 * instr.fullOparg(py_instr));
            break;
        case BUILD_CONST_KEY_MAP:
            stack.push();
            stack.pop_n_consecutively(1 + instr.fullOparg(py_instr));
            break;
        case LIST_APPEND:
        case SET_ADD:
            stack.pop();
            break;
        case MAP_ADD:
            stack.pop();
            stack.pop();
            break;
        case LIST_EXTEND:
        case SET_UPDATE:
        case DICT_UPDATE:
        case DICT_MERGE:
            stack.pop();
            break;
        case LIST_TO_TUPLE:
            stack.push();
            stack.pop();
            break;

        case FORMAT_VALUE:
            stack.push();
            stack.pop();
            if ((instr.fullOparg(py_instr) & FVS_MASK) == FVS_HAVE_SPEC) {
                stack.pop();
            }
            break;
        case BUILD_STRING:
            stack.push();
            stack.pop_n_consecutively(instr.fullOparg(py_instr));
            break;

        case UNPACK_SEQUENCE: {
            auto n = instr.fullOparg(py_instr);
            while (n--) {
                stack.push();
            }
            stack.pop();
            break;
        }
        case UNPACK_EX: {
            auto oparg = instr.fullOparg(py_instr);
            auto n = (oparg & 0xff) + 1 + (oparg >> 8);
            while (n--) {
                stack.push();
            }
            stack.pop();
            break;
        }

        case GET_LEN:
        case MATCH_MAPPING:
        case MATCH_SEQUENCE:
        case MATCH_KEYS:
        case MATCH_CLASS:
        case COPY_DICT_WITHOUT_KEYS:

        case BUILD_SLICE:
        case LOAD_ASSERTION_ERROR:
        case RAISE_VARARGS:
        case SETUP_ANNOTATIONS:
        case PRINT_EXPR:
            break;

        case SETUP_FINALLY:
        case POP_BLOCK:
        case POP_EXCEPT:
            break;
        case JUMP_IF_NOT_EXC_MATCH:
            stack.pop();
            stack.pop();
            break;
        case RERAISE:
        case SETUP_WITH:
            stack.push();
            stack.push();
            stack.pop();
            break;
        case WITH_EXCEPT_START:
            stack.push();
            break;

        case GEN_START:
        case YIELD_VALUE:
        case GET_YIELD_FROM_ITER:
        case YIELD_FROM:
        case GET_AWAITABLE:
        case GET_AITER:
        case GET_ANEXT:
        case END_ASYNC_FOR:
        case SETUP_ASYNC_WITH:
        case BEFORE_ASYNC_WITH:
        default:
            break;
        }
    }
}

void CompileUnit::analyzeLocalsDefinition() {
    auto &worklist = pending_block_indices;
    worklist.reserve(block_num);

    for (auto i : IntRange(1, block_num)) {
        auto &b = blocks[i];
        b.in_worklist = true;
        worklist.push(i);
        b.locals_input.fill(py_code->co_nlocals, true);
    }
    blocks[0].locals_input.fill(py_code->co_nlocals);
    blocks[0].in_worklist = true;
    worklist.push(0);

    auto nargs = py_code->co_argcount + (py_code->co_flags & CO_VARARGS)
            + py_code->co_kwonlyargcount + (py_code->co_flags & CO_VARKEYWORDS);
    for (auto i : IntRange(nargs)) {
        blocks[0].locals_input.set(i);
    }

    BitArray block_output(py_code->co_nlocals);
    auto chunk_num = BitArray::chunkNumber(py_code->co_nlocals);
    auto update_successor = [&](unsigned index) {
        auto &successor = blocks[index];
        bool any_update = false;
        for (auto i : IntRange(chunk_num)) {
            auto &chunk = successor.locals_input.getChunk(i);
            auto old_chunk = chunk;
            chunk = old_chunk & block_output.getChunk(i);
            any_update |= chunk != old_chunk;
        }
        if (any_update && !successor.in_worklist) {
            successor.in_worklist = true;
            worklist.push(index);
        }
    };

    while (!worklist.empty()) {
        auto &b = blocks[worklist.pop()];
        b.in_worklist = false;
        for (auto i : IntRange(chunk_num)) {
            // TODO: 名不副实
            block_output.getChunk(i) = (b.locals_input.getChunk(i) & b.locals_touched.getChunk(i))
                    | b.locals_deleted.getChunk(i);
        }
        if (b.fall_through) {
            update_successor(&b - &*blocks + 1);
        }
        if (b.has_branch) {
            update_successor(b.branch);
        }
    }
}

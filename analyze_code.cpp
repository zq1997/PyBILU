#include <iostream>

#include "compile_unit.h"

using namespace std;


constexpr bool isTerminator(int opcode) {
    switch (opcode) {
    case RETURN_VALUE:
    case RERAISE:
    case RAISE_VARARGS:
    case JUMP_ABSOLUTE:
    case JUMP_FORWARD:
        return true;
    default:
        return false;
    }
}

constexpr bool isAbsoluteJmp(int opcode) {
    switch (opcode) {
    case JUMP_IF_TRUE_OR_POP:
    case JUMP_IF_FALSE_OR_POP:
    case POP_JUMP_IF_TRUE:
    case POP_JUMP_IF_FALSE:
    case JUMP_IF_NOT_EXC_MATCH:
    case JUMP_ABSOLUTE:
        return true;
    default:
        return false;
    }
}

constexpr bool isRelativeJump(int opcode) {
    switch (opcode) {
    case FOR_ITER:
    case JUMP_FORWARD:
        return true;
    default:
        return false;
    }
}

constexpr bool isTryBlockSetup(int opcode) {
    switch (opcode) {
    case SETUP_FINALLY:
    case SETUP_WITH:
    case SETUP_ASYNC_WITH:
        return true;
    default:
        return false;
    }
}

void CompileUnit::parseCFG() {
    auto size = PyBytes_GET_SIZE(py_code->co_code) / sizeof(_Py_CODEUNIT);
    vpc_to_stack_height.reserve(size);
    redundant_loads.reserve(size, false);

    BitArray is_boundary(size + 1);
    block_num = 0;

    const PyInstrPointer py_instr{py_code};
    for (auto vpc : IntRange(size)) {
        auto instr = py_instr + vpc;
        auto opcode = instr.opcode();
        if (opcode == POP_BLOCK) {
            block_num += is_boundary.set(vpc + 1);
        } else {
            auto is_try_setup = isTryBlockSetup(opcode);
            auto is_rel_jmp = isRelativeJump(opcode);
            auto is_abs_jmp = isAbsoluteJmp(opcode);
            try_block_num += is_try_setup;
            if (is_try_setup || is_rel_jmp || is_abs_jmp) {
                auto dest = instr.oparg(py_instr) + (is_abs_jmp ? 0 : vpc + 1);
                block_num += is_boundary.set(dest) + (!isTerminator(opcode) && is_boundary.set(vpc + 1));
            }
        }
    }

    block_num -= is_boundary.get(0);
    is_boundary.reset(0);
    block_num += is_boundary.set(size);

    blocks.reserve(block_num + try_block_num);
    pending_block_indices.reserve(block_num + try_block_num);

    unsigned created_block_num = 0;
    unsigned start = 0;
    for (auto i : IntRange(BitArray::chunkNumber(size + 1))) {
        unsigned j = 0;
        for (auto bits = is_boundary.getChunk(i); bits; bits >>= 1) {
            if (bits & 1) {
                auto end = i * BitArray::BitsPerValue + j;
                auto &b = blocks[created_block_num++];
                b.start = start;
                b.block = createBlock(useName("block_for_instr_.", start));
                auto tail_instr = py_instr + (end - 1);
                auto opcode = tail_instr.opcode();
                if (opcode == POP_BLOCK) {
                    b.fall_through = true;
                    b.has_branch = false;
                    b.try_block_enter = false;
                    b.try_block_exit = true;
                } else {
                    b.fall_through = !isTerminator(opcode);
                    auto is_try_setup = isTryBlockSetup(opcode);
                    auto is_rel_jmp = isRelativeJump(opcode);
                    auto is_abs_jmp = isAbsoluteJmp(opcode);
                    if ((b.has_branch = is_try_setup || is_rel_jmp || is_abs_jmp)) {
                        b.branch = tail_instr.oparg(py_instr) + (is_abs_jmp ? 0 : end);
                    }
                    b.try_block_enter = is_try_setup;
                    b.try_block_exit = false;
                }
                start = end;
            }
            j++;
        }
    }
    assert(created_block_num == block_num);

    auto nlocals = py_code->co_nlocals;
    auto exception_block_index = block_num;
    for (auto &b : PtrRange(&*blocks, block_num)) {
        b.locals_keep.reserve(nlocals, false);
        b.locals_set.reserve(nlocals, false);
        b.locals_ever_deleted.reserve(nlocals, false);
        if (b.has_branch) {
            auto branch = findPyBlock(b.branch);
            if (b.try_block_enter) {
                b.branch = exception_block_index++;
                auto &eb = blocks[b.branch];
                eb.has_branch = true;
                eb.fall_through = false;
                eb.try_block_enter = false;
                eb.try_block_exit = false;
                eb.branch = branch;
                eb.locals_keep.reserve(nlocals, true);
                eb.locals_set.reserve(nlocals, false);
                eb.locals_ever_deleted.reserve(nlocals, false);
            } else {
                b.branch = branch;
            }
        }
    }
    assert(exception_block_index == block_num + try_block_num);
}

template <typename T>
class ReversedStack {
    ssize_t size;
    T *const stack;
    T *sp;
    T until_now;

    static constexpr auto until_forever = numeric_limits<T>::max();

public:
    explicit ReversedStack(ssize_t size) : size{size}, stack{new T[size * 2]} { reset(); }

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

    DynamicArray<unsigned> locals(py_code->co_nlocals);
    for (auto i : IntRange(py_code->co_nlocals)) {
        locals[i] = until_finally;
    }

    auto block = &blocks[block_num - 1];
    for (auto timestamp = code_instr_num;;) {
        if (timestamp == block->start) {
            for (auto [k, s] : BitArrayChunks(py_code->co_nlocals, block->locals_keep, block->locals_set)) {
                s = ~s & k;
                k = ~k;
            }
            if (!timestamp) {
                break;
            }
            --block;
            stack.reset();
        }

        auto &locals_touched = block->locals_keep;
        auto &locals_deleted = block->locals_set;
        auto &locals_ever_deleted = block->locals_ever_deleted;

        stack.setTimestamp(--timestamp);
        auto instr = py_instr + timestamp;

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
            auto n = instr.oparg(py_instr);
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
            redundant_loads.setIf(timestamp, until_finally > stack.push());
            break;
        case LOAD_FAST: {
            auto oparg = instr.oparg(py_instr);
            redundant_loads.setIf(timestamp, locals[oparg] > stack.push());
            locals_touched.set(oparg);
            break;
        }
        case STORE_FAST: {
            auto oparg = instr.oparg(py_instr);
            locals[oparg] = timestamp;
            stack.pop();
            locals_touched.set(oparg);
            break;
        }
        case DELETE_FAST: {
            auto oparg = instr.oparg(py_instr);
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
            stack.pop_n_consecutively(1 + instr.oparg(py_instr));
            break;
        case CALL_FUNCTION_KW:
            stack.push();
            stack.pop_n_consecutively(2 + instr.oparg(py_instr));
            break;
        case CALL_FUNCTION_EX:
            stack.push();
            stack.pop();
            stack.pop();
            if (instr.oparg(py_instr) & 1) {
                stack.pop();
            }
            break;
        case CALL_METHOD:
            stack.push();
            stack.pop_n_consecutively(2 + instr.oparg(py_instr));
            break;
        case LOAD_CLOSURE:
            stack.push();
            break;
        case MAKE_FUNCTION: {
            stack.push();
            auto oparg = instr.oparg(py_instr);
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
            stack.pop_n_consecutively(instr.oparg(py_instr));
            break;
        case BUILD_MAP:
            stack.push();
            stack.pop_n_consecutively(2 * instr.oparg(py_instr));
            break;
        case BUILD_CONST_KEY_MAP:
            stack.push();
            stack.pop_n_consecutively(1 + instr.oparg(py_instr));
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
            if ((instr.oparg(py_instr) & FVS_MASK) == FVS_HAVE_SPEC) {
                stack.pop();
            }
            break;
        case BUILD_STRING:
            stack.push();
            stack.pop_n_consecutively(instr.oparg(py_instr));
            break;

        case UNPACK_SEQUENCE: {
            auto n = instr.oparg(py_instr);
            while (n--) {
                stack.push();
            }
            stack.pop();
            break;
        }
        case UNPACK_EX: {
            auto oparg = instr.oparg(py_instr);
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

void collect_deleted_locals(BitArray &keep, PyBasicBlock blocks[], unsigned nested_try_depth, unsigned epoch, unsigned nlocals, unsigned i_b) {
    auto &b = blocks[i_b];
    if (b.try_block_epoch >= epoch) {
        return;
    }
    b.try_block_epoch = epoch;
    for (auto [k, d] : BitArrayChunks(nlocals, keep, b.locals_ever_deleted)) {
        k &= ~d;
    }
    if (b.try_block_enter) {
        collect_deleted_locals(keep, blocks, nested_try_depth + 1, epoch, nlocals, i_b + 1);
        collect_deleted_locals(keep, blocks, nested_try_depth + 1, epoch, nlocals, blocks[b.branch].branch);
        return;
    }
    if (b.try_block_exit) {
        if (nested_try_depth == 1) {
            return;
        }
        collect_deleted_locals(keep, blocks, nested_try_depth - 1, epoch, nlocals, i_b + 1);
        return;
    }
    if (b.has_branch) {
        collect_deleted_locals(keep, blocks, nested_try_depth, epoch, nlocals, b.branch);
    }
    if (b.fall_through) {
        collect_deleted_locals(keep, blocks, nested_try_depth, epoch, nlocals, i_b + 1);
    }
}

void CompileUnit::analyzeLocalsDefinition() {
    unsigned epoch = 0;
    for (auto &b : PtrRange(&blocks[0], block_num)) {
        if (b.try_block_enter) {
            collect_deleted_locals(blocks[b.branch].locals_keep, &blocks[0], 1, ++epoch, py_code->co_nlocals, &b - &blocks[0] + 1);
        }
    }

    PyBasicBlock *worklist_head = nullptr;
    for (auto i : IntRange(1, block_num + try_block_num)) {
        auto &b = blocks[i];
        b.worklist_next = worklist_head;
        b.locals_input.fill(py_code->co_nlocals, true);
        worklist_head = &b;
    }

    blocks[0].locals_input.fill(py_code->co_nlocals);
    blocks[0].worklist_next = worklist_head;
    worklist_head = &blocks[0];
    auto nargs = py_code->co_argcount + (py_code->co_flags & CO_VARARGS)
            + py_code->co_kwonlyargcount + (py_code->co_flags & CO_VARKEYWORDS);
    for (auto i : IntRange(nargs)) {
        blocks[0].locals_input.set(i);
    }

    BitArray block_output(py_code->co_nlocals);
    PyBasicBlock *const marked_as_not_in_worklist = &*blocks + (block_num + try_block_num);
    auto update_successor = [&](unsigned index) {
        auto &successor = blocks[index];
        bool any_update = false;
        for (auto [x, y] : BitArrayChunks(py_code->co_nlocals, block_output, successor.locals_input)) {
            auto old_y = y;
            y &= x;
            any_update |= y != old_y;
        }
        if (any_update && successor.worklist_next == marked_as_not_in_worklist) {
            successor.worklist_next = worklist_head;
            worklist_head = &successor;
        }
    };

    do {
        auto &b = *worklist_head;
        worklist_head = b.worklist_next;
        b.worklist_next = marked_as_not_in_worklist;
        for (auto [i, o, mask, set] : BitArrayChunks(py_code->co_nlocals, b.locals_input, block_output,
                b.locals_keep, b.locals_set)) {
            o = (i & mask) | set;
        }
        if (b.fall_through) {
            update_successor(&b - &*blocks + 1);
        }
        if (b.has_branch) {
            update_successor(b.branch);
        }
    } while (worklist_head);

    for (auto i : IntRange(py_code->co_nlocals)) {
        fprintf(stderr, "\t%s", PyUnicode_AsUTF8(PyTuple_GET_ITEM(py_code->co_varnames, i)));
    }
    for (auto &b : PtrRange(&blocks[0], block_num)) {
        if (b.try_block_enter) {
            b.branch = blocks[b.branch].branch;
        }
        fprintf(stderr, "\n%u", b.start);
        for (auto i : IntRange(py_code->co_nlocals)) {
            fprintf(stderr, b.locals_input.get(i) ? "\to" : "\tx");
        }
    }
    fprintf(stderr, "\n");
}

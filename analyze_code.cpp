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
    case POP_JUMP_IF_TRUE:
    case POP_JUMP_IF_FALSE:
    case JUMP_IF_TRUE_OR_POP:
    case JUMP_IF_FALSE_OR_POP:
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

auto *findPyBlock(PyBasicBlock *blocks, unsigned block_num, unsigned offset) {
    auto left = &blocks[0];
    auto right = &blocks[block_num - 2];
    while (left <= right) {
        auto mid = left + (right - left) / 2;
        auto v = mid->end_index;
        if (v < offset) {
            left = mid + 1;
        } else if (v > offset) {
            right = mid - 1;
        } else {
            return mid + 1;
        }
    }
    assert(offset == 0);
    return &blocks[0];
}

void CompileUnit::parseCFG() {
    auto py_instr_num = PyBytes_GET_SIZE(py_code->co_code) / sizeof(_Py_CODEUNIT);
    vpc_to_stack_height.reserve(py_instr_num);
    redundant_loads.reserve(py_instr_num, false);

    BitArray is_boundary(py_instr_num + 1);
    handler_num = 1;
    block_num = 0;
    try_block_num = 0;

    const PyInstrPointer py_instr{py_code};
    for (auto vpc : IntRange(py_instr_num)) {
        auto instr = py_instr + vpc;
        auto opcode = instr.opcode();
        handler_num += opcode == YIELD_VALUE;
        if (opcode == POP_BLOCK) {
            block_num += is_boundary.checkAndSet(vpc + 1);
        } else {
            auto is_try_setup = isTryBlockSetup(opcode);
            auto is_rel_jmp = isRelativeJump(opcode);
            auto is_abs_jmp = isAbsoluteJmp(opcode);
            try_block_num += is_try_setup;
            if (is_try_setup || is_rel_jmp || is_abs_jmp) {
                auto dest = instr.oparg(py_instr) + (is_abs_jmp ? 0 : vpc + 1);
                block_num += is_boundary.checkAndSet(dest);
                block_num += !isTerminator(opcode) && is_boundary.checkAndSet(vpc + 1);
            }
        }
    }

    block_num += is_boundary.checkAndSet(py_instr_num);
    block_num -= is_boundary.get(0);
    is_boundary.reset(0);

    blocks.reserve(block_num + try_block_num);

    unsigned created_block_num = 0;
    unsigned start_index = 0;
    for (auto i : IntRange(BitArray::chunkNumber(py_instr_num + 1))) {
        unsigned j = 0;
        for (auto bits = is_boundary.getChunk(i); bits; bits >>= 1) {
            if (bits & 1) {
                auto &b = blocks[created_block_num++];
                b.end_index = i * BitArray::bits_per_chunk + j;
                b.block = createBlock(useName("instr.", start_index));
                auto tail_instr = py_instr + (b.end_index - 1);
                auto opcode = tail_instr.opcode();
                if (opcode == POP_BLOCK) {
                    b.fall_through = true;
                    b._branch_offset = py_instr_num;
                    b.eh_body_exit = opcode == POP_BLOCK;
                } else {
                    b.fall_through = !isTerminator(opcode);
                    auto is_try_setup = isTryBlockSetup(opcode);
                    auto is_rel_jmp = isRelativeJump(opcode);
                    auto is_abs_jmp = isAbsoluteJmp(opcode);
                    if (is_try_setup || is_rel_jmp || is_abs_jmp) {
                        b._branch_offset = tail_instr.oparg(py_instr) + (is_abs_jmp ? 0 : b.end_index);
                    } else {
                        b._branch_offset = py_instr_num;
                    }
                    b.eh_body_enter = is_try_setup;
                    handler_num += is_try_setup;
                }
                start_index = b.end_index;
            }
            j++;
        }
    }
    assert(created_block_num == block_num);

    auto nlocals = py_code->co_nlocals;
    auto exception_block_index = block_num;
    for (auto &b : PtrRange(blocks.getPointer(), block_num)) {
        b.locals_kept.reserve(nlocals, false);
        b.locals_set.reserve(nlocals, false);
        b.locals_ever_deleted.reserve(nlocals, false);
        if (b._branch_offset != py_instr_num) {
            auto branch_block = findPyBlock(blocks.getPointer(), block_num, b._branch_offset);
            if (b.eh_body_enter) {
                auto &eb = *(b.branch = &blocks[exception_block_index++]);
                eb.locals_kept.reserve(nlocals, true);
                eb.locals_set.reserve(nlocals, false);
                eb.locals_ever_deleted.reserve(nlocals, false);
                eb.branch = branch_block;
                eb.eh_setup_block = &b;
            } else {
                b.branch = branch_block;
            }
        } else {
            b.branch = nullptr;
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
public:

    static constexpr auto until_forever = numeric_limits<T>::max();

    explicit ReversedStack(ssize_t size) : size{size}, stack{new T[size * 2]} { reset(); }

    ~ReversedStack() noexcept { delete[] stack; }

    void setTimestamp(T t) { until_now = t; }

    int height() { return (stack + size) - sp; }

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


void CompileUnit::doIntraBlockAnalysis() {
    ReversedStack<unsigned> stack(py_code->co_stacksize);

    const PyInstrPointer py_instr{py_code};
    unsigned code_instr_num = blocks[block_num - 1].end_index;
    constexpr unsigned until_anytime = 0;

    DynamicArray<unsigned> locals(py_code->co_nlocals);
    for (auto i : IntRange(py_code->co_nlocals)) {
        locals[i] = code_instr_num;
    }

    auto block = &blocks[block_num - 1];
    auto stop_at_vpc = block == blocks.getPointer() ? 0 : block[-1].end_index;
    for (auto vpc = code_instr_num;;) {
        if (vpc == stop_at_vpc) {
            for (auto [k, s] : BitArrayChunks(py_code->co_nlocals, block->locals_kept, block->locals_set)) {
                s = ~s & k;
                k = ~k;
            }
            block->stack_effect = stack.height();
            if (!vpc) {
                break;
            }
            --block;
            stop_at_vpc = block == blocks.getPointer() ? 0 : block[-1].end_index;
            stack.reset();
        }

        auto &locals_touched = block->locals_kept;
        auto &locals_deleted = block->locals_set;
        auto &locals_ever_deleted = block->locals_ever_deleted;

        stack.setTimestamp(--vpc);
        auto instr = py_instr + vpc;

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
            redundant_loads.setIf(vpc, code_instr_num > stack.push());
            break;
        case LOAD_FAST: {
            auto oparg = instr.oparg(py_instr);
            redundant_loads.setIf(vpc, locals[oparg] > stack.push());
            locals_touched.set(oparg);
            break;
        }
        case STORE_FAST: {
            auto oparg = instr.oparg(py_instr);
            locals[oparg] = vpc;
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
            stack.pop(stack.until_forever);
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
        case CALL_METHOD:
            stack.push();
            stack.pop_n_consecutively(2 + instr.oparg(py_instr));
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
            block->branch_stack_difference = 0;
            break;
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_FALSE:
            block->branch_stack_difference = 0;
            stack.pop();
            break;
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
            block->branch_stack_difference = 1;
            stack.pop(stack.until_forever);
            break;
        case GET_ITER:
            stack.push();
            stack.pop();
            break;
        case FOR_ITER:
            block->branch_stack_difference = -2;
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
            throw runtime_error("尚未实现");
            break;

        case SETUP_FINALLY:
            block->branch_stack_difference = 6;
            break;
        case POP_BLOCK:
            break;
        case POP_EXCEPT:
            stack.pop();
            stack.pop();
            stack.pop();
            break;
        case JUMP_IF_NOT_EXC_MATCH:
            block->branch_stack_difference = 0;
            stack.pop();
            stack.pop();
            break;
        case RERAISE:
        case SETUP_WITH:
            block->branch_stack_difference = 5;
            stack.push();
            stack.push();
            stack.pop(stack.until_forever); // TODO: 想办法直接返回
            break;
        case WITH_EXCEPT_START:
            stack.push();
            break;

        case GEN_START:
            stack.pop();
            break;
        case YIELD_VALUE:
            stack.push();
            stack.pop();
            break;
        case GET_YIELD_FROM_ITER:
            stack.push();
            stack.pop();
            break;
        case YIELD_FROM:
            stack.push();
            stack.pop();
            stack.pop(stack.until_forever);
            break;
        case GET_AWAITABLE:
            stack.push();
            stack.pop();
            break;
        case GET_AITER:
            stack.push();
            stack.pop();
            break;
        case GET_ANEXT:
            stack.push();
            break;
        case END_ASYNC_FOR:
            stack.pop_n_consecutively(7);
            break;
        case SETUP_ASYNC_WITH:
            block->branch_stack_difference = -1 + 6;
            break;
        case BEFORE_ASYNC_WITH:
            stack.push();
            stack.push();
            stack.pop(stack.until_forever);
            break;
        default:
            Py_UNREACHABLE();
            break;
        }
    }
}

void analyzeEHBlock(PyBasicBlock &eb, unsigned nlocals, unsigned nesting_depth, PyBasicBlock &b) {
    if (b.visited_eh_block == &eb) {
        return;
    }
    b.visited_eh_block = &eb;
    for (auto [kept, deleted] : BitArrayChunks(nlocals, eb.locals_kept, b.locals_ever_deleted)) {
        kept &= ~deleted;
    }
    if (b.branch) {
        auto branch_block = b.eh_body_enter ? b.branch->branch : b.branch;
        analyzeEHBlock(eb, nlocals, nesting_depth, *branch_block);
    }
    nesting_depth = nesting_depth + b.eh_body_enter - b.eh_body_exit;
    if (b.fall_through && nesting_depth) {
        if (nesting_depth >= CO_MAXBLOCKS - 1) {
            throw runtime_error("too much nesting blocks");
        }
        analyzeEHBlock(eb, nlocals, nesting_depth, b.next());
    }
}

void CompileUnit::doInterBlockAnalysis() {
    const auto nlocals = py_code->co_nlocals;

    for (auto &eb : PtrRange(blocks.getPointer(block_num), try_block_num)) {
        analyzeEHBlock(eb, nlocals, 1, eb.eh_setup_block->next());
    }

    PyBasicBlock *worklist_head = nullptr;
    auto &first_block = blocks[0];
    for (auto b = blocks.getPointer(block_num + try_block_num); b-- != &first_block;) {
        b->worklist_link = worklist_head;
        b->locals_input.fill(nlocals, true);
        worklist_head = b;
    }
    auto nargs = py_code->co_argcount + (py_code->co_flags & CO_VARARGS)
            + py_code->co_kwonlyargcount + (py_code->co_flags & CO_VARKEYWORDS);
    for (auto [chunk] : BitArrayChunks(nlocals, first_block.locals_input)) {
        chunk &= (BitArray::ChunkType{1} << nargs) - 1U;
        nargs = nargs > BitArray::bits_per_chunk ? nargs - BitArray::bits_per_chunk : 0;
    }

    BitArray block_output(nlocals);

    do {
        auto &b = *worklist_head;
        worklist_head = b.worklist_link;
        b.worklist_link = &b; // mark it as not in worklist by pointing to self
        for (auto [input, output, kept, set] : BitArrayChunks(nlocals,
                b.locals_input, block_output, b.locals_kept, b.locals_set)) {
            output = (input & kept) | set;
        }

        for (auto successor : {b.branch, b.fall_through ? &b.next() : nullptr}) {
            if (successor) {
                bool any_update = false;
                for (auto [x, y] : BitArrayChunks(nlocals, block_output, successor->locals_input)) {
                    auto old_y = y;
                    y &= x;
                    any_update |= y != old_y;
                }
                if (any_update && successor->worklist_link == successor) {
                    successor->worklist_link = worklist_head;
                    worklist_head = successor;
                }
            }
        }
    } while (worklist_head);

    for (auto &b : PtrRange(blocks.getPointer(block_num), try_block_num)) {
        assert(b.eh_setup_block->branch == &b);
        b.eh_setup_block->branch = b.branch;
    }

    worklist_head = &blocks[0];
    worklist_head->initial_stack_height = !!(py_code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR));
    do {
        auto &b = *worklist_head;
        worklist_head = b.worklist_link;
        b.worklist_link = nullptr; // mark it as visited by pointing to nullptr

        for (auto [successor, difference] : std::initializer_list<std::tuple<PyBasicBlock *, int>>{
                {b.branch, b.branch_stack_difference},
                {b.fall_through ? &b.next() : nullptr, 0}
        }) {
            if (successor) {
                auto height = b.initial_stack_height + b.stack_effect + difference;
                assert(0 <= height && height <= py_code->co_stacksize);
                if (successor->worklist_link == successor) {
                    successor->initial_stack_height = height;
                    successor->worklist_link = worklist_head;
                    worklist_head = successor;
                } else {
                    assert(successor->initial_stack_height == height);
                }
            }
        }
    } while (worklist_head);

    // for (auto i : IntRange(nlocals)) {
    //     fprintf(stderr, "\t%s", PyUnicode_AsUTF8(PyTuple_GET_ITEM(py_code->co_varnames, i)));
    // }
    // unsigned start_index = 0;
    // for (auto &b : PtrRange(&blocks[0], block_num)) {
    //     fprintf(stderr, "\n%u->%u", start_index, b.end_index);
    //     for (auto i : IntRange(nlocals)) {
    //         fprintf(stderr, b.locals_input.get(i) ? "\to" : "\tx");
    //     }
    //     start_index = b.end_index;
    // }
    // fprintf(stderr, "\n");
}

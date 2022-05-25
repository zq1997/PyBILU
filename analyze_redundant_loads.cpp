#include <iostream>

#include "compile_unit.h"

using namespace std;

template <typename T>
class ReversedStack {
    ssize_t size;
    T *const stack;
    T *sp;
    T timestamp;

    static constexpr auto infinity = numeric_limits<T>::max();

public:
    explicit ReversedStack(ssize_t size) : size{size}, stack{new T[size * 2]}, sp{stack + size} {
        reset();
    }

    ~ReversedStack() noexcept { delete[] stack; }

    void setTimestamp(T t) { timestamp = t; }

    void reset() {
        sp = stack + size;
        for (auto i : Range(size)) {
            stack[i] = infinity;
        }
    }

    void consume() {
        *sp++ = timestamp;
    }

    T produce() {
        return *--sp;
    }

    bool produceWithTest(T value) {
        if (sp == stack) {
            return false;
        }
        return value >= *--sp;
    }
};


void CompileUnit::analyzeRedundantLoads() {
    ReversedStack<unsigned> stack(py_code->co_stacksize);

    assert(block_num >= 1);
    auto current_block = &blocks[block_num - 1];
    auto block_first_instr_index = current_block[-1].end;

    const PyInstrPointer py_instr{py_code};
    unsigned code_instr_num = PyBytes_GET_SIZE(py_code->co_code) / sizeof(PyInstr);
    const auto default_expiration = code_instr_num - 1;

    BitArray redundant_loads(code_instr_num);
    DynamicArray<unsigned> locals(py_code->co_nlocals);
    for (auto i : Range(py_code->co_nlocals)) {
        locals[i] = default_expiration;
    }

    for (auto index = code_instr_num;;) {
        assert(index);
        stack.setTimestamp(--index);
        auto instr = py_instr + index;

        switch (instr.opcode()) {
        case DUP_TOP:
            stack.produce();
            stack.produce();
            stack.consume();
        case LOAD_CONST:
            redundant_loads.setIf(index, default_expiration > stack.produce());
            break;
        case LOAD_FAST:
            redundant_loads.setIf(index, locals[instr.fullOparg(py_instr)] > stack.produce());
            break;
        case STORE_FAST:
            locals[instr.fullOparg(py_instr)] = index;
            stack.consume();
            break;
        case GET_ITER:
            stack.produce();
            stack.consume();
        case FOR_ITER:
            stack.consume();
            break;
        case BINARY_ADD:
            stack.produce();
            stack.consume();
            stack.consume();
            break;
        case JUMP_ABSOLUTE:
            break;
        case RETURN_VALUE:
            stack.consume();
            break;
        default:
            break;
        }
        if (index == block_first_instr_index) {
            if (index == 0) {
                break;
            }
            --current_block;
            block_first_instr_index = current_block[-1].end;
            stack.reset();
        }
    }
}

#include <iostream>

#include <opcode.h>

#include "JIT.h"

using namespace std;

class BitArray {
public:
    using EleType = unsigned;
    static constexpr auto EleBits = CHAR_BIT * sizeof(EleType);

    explicit BitArray(size_t s) : size(s), data(new EleType[s / EleBits + !!(s % EleBits)]()) {}

    void set(size_t index) {
        data.get()[index / EleBits] |= EleType{1} << index % EleBits;
    }

    bool get(size_t index) {
        return data.get()[index / EleBits] & (EleType{1} << index % EleBits);
    }

    auto count() {
        size_t counted = 0;
        for (auto i = size / EleBits + !!(size % EleBits); i--;) {
            counted += bitset<EleBits>(data.get()[i]).count();
        }
        return counted;
    }

private:
    size_t size;
    unique_ptr<EleType> data;
};

void MyJIT::parseCFG(PyCodeObject *cpy_ir) {
    PyInstrIter iter(cpy_ir);
    auto is_boundary = BitArray(iter.getSize() + 1);
    is_boundary.set(0);

    PyOpcode opcode;
    PyOparg oparg;
    while (iter.next(opcode, oparg)) {
        switch (opcode) {
        case EXTENDED_ARG:
            oparg <<= EXTENDED_ARG_BITS;
            continue;
        case JUMP_ABSOLUTE:
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_FALSE:
        case JUMP_IF_NOT_EXC_MATCH:
            is_boundary.set(iter.getOffset());
            is_boundary.set(oparg);
            break;
        case JUMP_FORWARD:
        case FOR_ITER:
            is_boundary.set(iter.getOffset());
            is_boundary.set(iter.getOffset() + oparg / sizeof(PyInstr));
            break;
        case RETURN_VALUE:
        case RAISE_VARARGS:
        case RERAISE:
            is_boundary.set(iter.getOffset());
        default:
            break;
        }
    }

    assert(is_boundary.get(iter.getSize()));
    cout << "number of basic blocks: " << is_boundary.count() - 1 << endl;
}


void MyJIT::addInstr(PyOpcode opcode, PyOparg oparg) {
}

#include <iostream>

#include <opcode.h>

#include "JIT.h"

using namespace std;
using namespace llvm;


struct SymbolTable {
    void *PyNumber_Add{reinterpret_cast<void *>(::PyNumber_Add)};
    void *PyNumber_Multiply{reinterpret_cast<void *>(::PyNumber_Multiply)};
} extern const symbol_table{};

unique_ptr<Translator> MyJIT::parseCFG(PyCodeObject *cpy_ir) {
    PyInstrIter iter(cpy_ir);
    BitArray is_boundary(iter.getSize() + 1);

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

    is_boundary.set(0, false);
    assert(is_boundary.get(iter.getSize()));
    return make_unique<Translator>(is_boundary, reinterpret_cast<PyInstr *>(PyBytes_AS_STRING(cpy_ir->co_code)));
}


Translator::Translator(const BitArray &boundary_bit_array, PyInstr *py_instructions_) :
        block_num(boundary_bit_array.count()),
        boundaries(new unsigned[block_num]),
        ir_blocks(new BasicBlock *[block_num]),
        py_instructions(py_instructions_) {
    unsigned current_num = 0;
    auto chunks = boundary_bit_array.chunkNumber();
    for (decltype(chunks) i = 0; i < chunks; i++) {
        auto bits = boundary_bit_array.data.get()[i];
        BitArray::EleType tester{1};
        for (unsigned j = 0; j < BitArray::EleBits; j++) {
            if (tester & bits) {
                boundaries.get()[current_num++] = i * BitArray::EleBits + j;
            }
            tester <<= 1;
        }
    }
    assert(current_num == block_num);
}

CallInst *createCall(MyJIT &jit, Value *table, void *const &func, Type *result,
                     ArrayRef<Type *> arg_types, initializer_list<Value *> args) {
    auto bf_type = FunctionType::get(result, arg_types, false);
    auto offset = reinterpret_cast<const char *>(&func) - reinterpret_cast<const char *>(&symbol_table);
    auto entry = jit.builder.CreateInBoundsGEP(jit.type_char, table,
                                               ConstantInt::get(jit.type_ptrdiff, offset));
    entry = jit.builder.CreatePointerCast(entry, bf_type->getPointerTo()->getPointerTo());
    entry = jit.builder.CreateLoad(bf_type->getPointerTo(), entry);
    return jit.builder.CreateCall(bf_type, entry, args);
}

void Translator::emit_block(unsigned index) {
    auto first = index ? boundaries.get()[index - 1] : 0;
    auto last = boundaries.get()[index];
    PyInstrIter iter(py_instructions + first, last - first);

    auto l = jit->builder.CreateLoad(py_obj, jit->builder.CreateConstInBoundsGEP1_32(py_obj, py_fast_locals, 0));
    auto r = jit->builder.CreateLoad(py_obj, jit->builder.CreateConstInBoundsGEP1_32(py_obj, py_fast_locals, 1));
    Type *arg_types[]{py_obj, py_obj};
    auto ll = createCall(*jit, py_symbol_table, symbol_table.PyNumber_Multiply, py_obj, arg_types, {l, l});
    auto rr = createCall(*jit, py_symbol_table, symbol_table.PyNumber_Multiply, py_obj, arg_types, {r, r});
    auto ret = createCall(*jit, py_symbol_table, symbol_table.PyNumber_Add, py_obj, arg_types, {ll, rr});
    jit->builder.CreateRet(ret);

    // PyOpcode opcode;
    // PyOparg oparg;
    // while (iter.next(opcode, oparg)) {
    //     switch (opcode) {
    //         // case EXTENDED_ARG:
    //         //     oparg <<= EXTENDED_ARG_BITS;
    //         //     continue;
    //         // case JUMP_ABSOLUTE:
    //         // case JUMP_IF_TRUE_OR_POP:
    //         // case JUMP_IF_FALSE_OR_POP:
    //         // case POP_JUMP_IF_TRUE:
    //         // case POP_JUMP_IF_FALSE:
    //         // case JUMP_IF_NOT_EXC_MATCH:
    //         //     is_boundary.set(iter.getOffset());
    //         //     is_boundary.set(oparg);
    //         //     break;
    //         // case JUMP_FORWARD:
    //         // case FOR_ITER:
    //         //     is_boundary.set(iter.getOffset());
    //         //     is_boundary.set(iter.getOffset() + oparg / sizeof(PyInstr));
    //         //     break;
    //         // case RETURN_VALUE:
    //         // case RAISE_VARARGS:
    //         // case RERAISE:
    //         //     is_boundary.set(iter.getOffset());
    //         default:
    //             break;
    //     }
    // }
}


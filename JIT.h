#include <climits>
#include <cstddef>
#include <memory>
#include <bitset>

#include <Python.h>
#include <opcode.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm-c/Target.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/MC/SubtargetFeature.h>

using PyInstr = const _Py_CODEUNIT;
using PyOpcode = decltype(_Py_OPCODE(PyInstr{}));
using PyOparg = decltype(_Py_OPCODE(PyInstr{}));
constexpr auto EXTENDED_ARG_BITS = 8;

struct SymbolTable {
    void *PyNumber_Add{reinterpret_cast<void *>(::PyNumber_Add)};
    void *PyNumber_Multiply{reinterpret_cast<void *>(::PyNumber_Multiply)};
} extern const symbol_table;

class Compiler {
    std::unique_ptr<llvm::TargetMachine> machine;

    llvm::ModuleAnalysisManager opt_MAM;
    llvm::FunctionAnalysisManager opt_FAM;
    llvm::FunctionPassManager opt_FPM;

    llvm::legacy::PassManager out_PM;
    llvm::SmallVector<char> out_vec;
    llvm::raw_svector_ostream out_stream{out_vec};

public:
    Compiler();

    void *operator()(llvm::Module &mod);
};

class Translator {
public:
    llvm::LLVMContext context{};
    llvm::IRBuilder<> builder{context};

    llvm::Type *ctype_char{llvm::Type::getIntNTy(context, CHAR_BIT)};
    llvm::Type *ctype_ptr{ctype_char->getPointerTo()};
    llvm::Type *ctype_ptrdiff{llvm::Type::getIntNTy(context, CHAR_BIT * sizeof(std::ptrdiff_t))};

    llvm::Module mod{"", context};

    llvm::Function *func{llvm::Function::Create(
            llvm::FunctionType::get(ctype_ptr, {
                    ctype_ptr,
                    ctype_ptr->getPointerTo()
            }, false),
            llvm::Function::ExternalLinkage,
            "",
            &mod
    )};

    PyInstr *py_instructions{};

    unsigned block_num{};
    std::unique_ptr<unsigned> boundaries{};
    std::unique_ptr<llvm::BasicBlock *> ir_blocks{};

    llvm::Value *py_symbol_table{func->getArg(0)};
    llvm::Value *py_fast_locals{func->getArg(1)};
    llvm::Value *py_stack{};

    void parseCFG(PyCodeObject *cpy_ir);

    void emitBlock(unsigned index);

public:
    Translator() {
        func->setAttributes(llvm::AttributeList::get(
                context, llvm::AttributeList::FunctionIndex,
                llvm::AttrBuilder()
                        .addAttribute(llvm::Attribute::NoUnwind)
                        .addAttribute("tune-cpu", llvm::sys::getHostCPUName())
        ));
    }

    void *operator()(Compiler &compiler, PyCodeObject *cpy_ir);
};

class PyInstrIter {
    const PyInstr *base;
    const size_t size;
    size_t offset{0};
public:
    PyInstrIter(const PyInstr *base_, size_t size_) : base(base_), size(size_) {}

    auto getOffset() const { return offset; }

    bool next(PyOpcode &opcode, PyOparg &oparg) {
        if (offset == size) {
            return false;
        }
        do {
            assert(offset < size);
            opcode = _Py_OPCODE(base[offset]);
            oparg = _Py_OPCODE(base[offset]);
            offset++;
        } while (opcode == EXTENDED_ARG);
        return true;
    }
};


class BitArray {
public:
    using EleType = unsigned long;
    static constexpr auto EleBits = CHAR_BIT * sizeof(EleType);

    explicit BitArray(size_t s) : size(s), data(new EleType[s / EleBits + !!(s % EleBits)]()) {}

    void set(size_t index, bool value = true) {
        data.get()[index / EleBits] |= EleType{value} << index % EleBits;
    }

    bool get(size_t index) {
        return data.get()[index / EleBits] & (EleType{1} << index % EleBits);
    }

    auto chunkNumber() const { return size / EleBits + !!(size % EleBits); };

    auto count() const {
        size_t counted = 0;
        for (auto i = chunkNumber(); i--;) {
            counted += std::bitset<EleBits>(data.get()[i]).count();
        }
        return counted;
    }

    const size_t size;
    const std::unique_ptr<EleType> data;
};

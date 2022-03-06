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

class MyJIT {
public:
    llvm::LLVMContext context{};
    llvm::IRBuilder<> builder{context};
    llvm::Module mod{"", context};

    llvm::ModuleAnalysisManager opt_MAM{};
    llvm::FunctionAnalysisManager opt_FAM{};
    llvm::FunctionPassManager opt_FPM{};

    llvm::legacy::PassManager out_PM;
    llvm::SmallVector<char> out_vec{};
    llvm::raw_svector_ostream out_stream{out_vec};

    llvm::AttributeList attrs{
            llvm::AttributeList::get(
                    context, llvm::AttributeList::FunctionIndex,
                    llvm::AttrBuilder()
                            .addAttribute(llvm::Attribute::NoUnwind)
                            .addAttribute("tune-cpu", llvm::sys::getHostCPUName())
            )
    };

    llvm::Type *type_char{llvm::Type::getIntNTy(context, CHAR_BIT)};
    llvm::Type *type_char_p{type_char->getPointerTo()};
    llvm::Type *type_ptrdiff{llvm::Type::getIntNTy(context, CHAR_BIT * sizeof(std::ptrdiff_t))};

    void compileFunction(llvm::Function *func, PyCodeObject *cpy_ir);

    static std::unique_ptr<class Translator> parseCFG(PyCodeObject *cpy_ir);

public:
    static void init();

    MyJIT();

    void *compile(PyCodeObject *cpy_ir);
};

class PyInstrIter {
    const PyInstr *base;
    const size_t size;
    size_t offset;
public:
    PyInstrIter(const PyInstr *base_, size_t size_) : base(base_), size(size_), offset(0) {}

    explicit PyInstrIter(PyCodeObject *code) : PyInstrIter(
            reinterpret_cast<PyInstr *>(PyBytes_AS_STRING(code->co_code)),
            PyBytes_GET_SIZE(code->co_code) / sizeof(PyInstr)) {}

    auto getSize() const { return size; }

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

class Translator {
    friend class MyJIT;

    const unsigned block_num;
    const std::unique_ptr<unsigned> boundaries;
    const std::unique_ptr<llvm::BasicBlock *> ir_blocks;
    PyInstr *py_instructions;

    MyJIT *jit{};
    llvm::Type *py_obj{};
    llvm::Value *py_symbol_table{};
    llvm::Value *py_fast_locals{};
    llvm::Value *py_stack{};

public:
    explicit Translator(const BitArray &bit_array, PyInstr *py_instructions);

    void emit_block(unsigned index);
};

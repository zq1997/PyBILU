#include <climits>
#include <cstddef>
#include <memory>
#include <list>

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
using PyOp = std::pair<PyOpcode, PyOparg>;
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

    static void parseCFG(PyCodeObject *cpy_ir);

    void addInstr(PyOpcode opcode, PyOparg oparg);

public:
    static void init();

    MyJIT();

    void *compile(PyCodeObject *cpy_ir);
};

class PyInstrIter {
    const PyInstr *first;
    const size_t size;
    size_t offset;
public:
    explicit PyInstrIter(PyCodeObject *code) :
            first(reinterpret_cast<PyInstr *>(PyBytes_AS_STRING(code->co_code))),
            size(PyBytes_GET_SIZE(code->co_code) / sizeof(PyInstr)),
            offset(0) {}

    auto getSize() const { return size; }

    auto getOffset() const { return offset; }

    bool next(PyOpcode &opcode, PyOparg &oparg) {
        if (offset == size) {
            return false;
        }
        do {
            assert(offset < size);
            opcode = _Py_OPCODE(first[offset]);
            oparg = _Py_OPCODE(first[offset]);
            offset++;
        } while (opcode == EXTENDED_ARG);
        return true;
    }
};

class NewPyInstr {
private:
    char *the_begin;
    char *the_end;
public:
    class PyInstrIter {
    private:
        char *the_current;
    public:
        explicit PyInstrIter(char *cur) : the_current(cur) {}

        PyInstrIter &operator++() {
            the_current += sizeof(PyInstr);
            return *this;
        }

        std::pair<PyOpcode, PyOparg> operator*() {
            auto instr = *reinterpret_cast<_Py_CODEUNIT *>(the_current);
            return {_Py_OPCODE(instr), _Py_OPARG(instr)};
        }

        bool operator!=(PyInstrIter other) const { return the_current != other.the_current; }
    };

    explicit NewPyInstr(PyCodeObject *code) :
            the_begin(PyBytes_AS_STRING(code->co_code)),
            the_end(the_begin + PyBytes_GET_SIZE(code->co_code)) {}

    PyInstrIter begin() { return PyInstrIter(the_begin); }

    PyInstrIter end() { return PyInstrIter(the_end); }
};
// class BasicBlock {
// public:
//     PyInstr *begin;
//     PyInstr *end;
//     BasicBlock *fall_to;
//     BasicBlock *jump_to;
// };
//
// class BasicBlocks {
// private:
//     std::list<BasicBlock> blocks;
// public:
//     BasicBlock &append() {
//         return blocks.emplace_back();
//     }
// };

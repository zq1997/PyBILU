#include <climits>
#include <cstddef>
#include <memory>
#include <bitset>
#include <tuple>
#include <array>
#include <type_traits>
#include <algorithm>

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
#include <llvm/Support/Memory.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/MC/SubtargetFeature.h>

#include "shared_symbols.h"
#include "general_utilities.h"

template <typename T1, typename T2>
inline auto dataDistance(T1 &from, T2 &to) {
    return reinterpret_cast<const char *>(&to) - reinterpret_cast<const char *>(&from);
}

class Compiler {
    std::unique_ptr<llvm::TargetMachine> machine{};

    llvm::ModuleAnalysisManager opt_MAM{};
    llvm::FunctionAnalysisManager opt_FAM{};
    llvm::LoopAnalysisManager opt_LAM{};
    llvm::FunctionPassManager opt_FPM{};

    llvm::legacy::PassManager out_PM{};
    llvm::SmallVector<char> out_vec{};
    llvm::raw_svector_ostream out_stream{out_vec};

public:
    Compiler();

    void *operator()(llvm::Module &mod);
};

class Translator {
    llvm::LLVMContext context{};
    RegisteredLLVMTypes types{(context.enableOpaquePointers(), context)};
    llvm::IRBuilder<> builder{context};
    llvm::Module mod{"", context};
    llvm::Function *func{llvm::Function::Create(
            LLVMType<PyObject *(PyObject *, PyObject **, PyObject **)>(context)(),
            llvm::Function::ExternalLinkage,
            "",
            &mod
    )};

    PyInstr *py_instructions{};

    unsigned block_num{};
    DynamicArray<unsigned> boundaries{};
    DynamicArray<llvm::BasicBlock *> ir_blocks{};

    llvm::Value *py_symbol_table{func->getArg(0)};
    llvm::Value *py_fast_locals{func->getArg(1)};
    llvm::Value *py_consts{func->getArg(2)};
    llvm::Value *py_stack{};
    std::ptrdiff_t stack_height{};

    template <typename T=char>
    auto getPointer(llvm::Value *base, ptrdiff_t index) {
        auto offset_value = castToLLVMValue(sizeof(T) * index, types);
        return builder.CreateInBoundsGEP(types.get<char>(), base, offset_value);
    }

    template <typename T, typename M>
    auto getPointer(llvm::Value *instance, M T::* member) {
        T dummy;
        auto offset = reinterpret_cast<const char *>(&(dummy.*member)) - reinterpret_cast<const char *>(&dummy);
        return getPointer(instance, offset);
    }

    template <typename T>
    auto readData(llvm::Value *base, ptrdiff_t index) {
        return builder.CreateLoad(types.get<T>(), getPointer<T>(base, index));
    }

    template <typename T, typename M>
    auto readData(llvm::Value *instance, M T::* member) {
        return builder.CreateLoad(types.get<M>(), getPointer(instance, member));
    }

    void parseCFG(PyCodeObject *cpy_ir);
    void emitBlock(unsigned index);
    llvm::Value *do_GETLOCAL(PyOparg oparg);
    void do_SETLOCAL(PyOparg oparg, llvm::Value *value);
    llvm::Value *do_POP();
    void do_PUSH(llvm::Value *value);
    void do_Py_INCREF(llvm::Value *v);
    void do_Py_DECREF(llvm::Value *v);

    template <typename T, typename ...Args>
    llvm::CallInst *do_Call(llvm::Value *callee, Args... args) {
        // TODO: remove_pointer_t太丑了
        auto callee_type = types.get<std::remove_pointer_t<T>>();
        return builder.CreateCall(callee_type, callee, {args...});
    }

    template <typename T, typename ...Args>
    llvm::CallInst *do_Call(T *const SymbolTable::* entry, Args... args) {
        auto callee_type = types.get<T>();
        auto callee = readData(py_symbol_table, entry);
        return builder.CreateCall(callee_type, callee, {args...});
    }

    void do_if(llvm::Value *cond,
            const std::function<void()> &create_then,
            const std::function<void()> &create_else = {}) {
        auto b_then = llvm::BasicBlock::Create(context, "", func);
        auto b_end = llvm::BasicBlock::Create(context, "", func);
        auto b_else = create_else ? llvm::BasicBlock::Create(context, "", func) : b_end;
        builder.CreateCondBr(cond, b_then, b_end);
        builder.SetInsertPoint(b_then);
        create_then();
        builder.CreateBr(b_end);
        if (create_else) {
            builder.SetInsertPoint(b_else);
            create_else();
            builder.CreateBr(b_end);
        }
        builder.SetInsertPoint(b_end);
    }


public:
    Translator() {
        // llvm::Attribute::AttrKind x = llvm::Attribute::get(context, "tune-cpu", llvm::sys::getHostCPUName());
        func->setAttributes(
                llvm::AttributeList::get(context, llvm::AttributeList::FunctionIndex,
                        llvm::AttrBuilder(context)
                                .addAttribute(llvm::Attribute::NoUnwind)
                                .addAttribute("tune-cpu", llvm::sys::getHostCPUName())));
    }

    void *operator()(Compiler &compiler, PyCodeObject *cpy_ir);
};

class PyInstrIter {
    const PyInstr *base;
    const size_t size;
    size_t offset{0};

    class IterResult {
        const bool has_next{false};
    public:
        const PyOpcode opcode{};
        const PyOparg oparg{};

        IterResult(PyOpcode op, PyOparg arg) : has_next{true}, opcode{op}, oparg{arg} {}

        IterResult() = default;

        explicit operator bool() const {
            return has_next;
        }
    };

public:
    PyInstrIter(const PyInstr *base_, size_t size_) : base(base_), size(size_) {}

    [[nodiscard]] auto getOffset() const { return offset; }

    IterResult next() {
        PyOpcode opcode;
        PyOparg oparg = 0;
        if (offset == size) {
            return {};
        }
        do {
            assert(offset < size);
            opcode = _Py_OPCODE(base[offset]);
            oparg = oparg << EXTENDED_ARG_BITS | _Py_OPARG(base[offset]);
            offset++;
        } while (opcode == EXTENDED_ARG);
        return {opcode, oparg};
    }
};

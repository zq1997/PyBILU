#ifndef PYNIC_TRANSLATOR
#define PYNIC_TRANSLATOR

#include <Python.h>
#include <opcode.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Support/Host.h>

#include "shared_symbols.h"
#include "general_utilities.h"
#include "compiler.h"

#ifdef NDEBUG
constexpr auto debug_build = false;
#else
constexpr auto debug_build = true;
#endif

template <typename T>
auto useName(const T &arg) {
    if constexpr (debug_build) {
        if constexpr (std::is_convertible_v<T, std::string>) {
            return std::string(arg);
        }
        if constexpr (std::is_integral_v<T>) {
            return std::to_string(arg);
        }
        if constexpr (std::is_same_v<T, PyObject *>) {
            return std::string(PyUnicode_AsUTF8(arg));
        }
    } else {
        return llvm::Twine::createNull();
    }
}

template <typename T, typename... Ts>
auto useName(const T &arg, const Ts &... more) {
    if constexpr (debug_build) {
        return useName(arg) + useName(more...);
    } else {
        return llvm::Twine::createNull();
    }
}

using TranslatedFunctionType = PyObject *(decltype(symbol_addresses), PyObject **, PyObject **);

class PyBasicBlock {
public:
    unsigned end;
    llvm::BasicBlock *llvm_block;
};

class Translator {
    llvm::LLVMContext context{};
    RegisteredLLVMTypes types{(context.enableOpaquePointers(), context)};
    llvm::IRBuilder<> builder{context};
    llvm::Module mod{"", context};
    llvm::Function *func{llvm::Function::Create(
            LLVMType<TranslatedFunctionType>(context)(), llvm::Function::ExternalLinkage, "", &mod
    )};
    llvm::MDNode *likely_true{llvm::MDBuilder(context).createBranchWeights(INT32_MAX, 0)};

    PyCodeObject *py_code;
    PyInstr *py_instructions{};
    unsigned block_num{};
    DynamicArray<PyBasicBlock> blocks;
    DynamicArray<llvm::Value *> py_locals;
    DynamicArray<llvm::Value *> py_consts;
    DynamicArray<llvm::Value *> py_stack;
    llvm::BasicBlock *unwind_block{}; // TODO: lazy

    llvm::Constant *null{llvm::ConstantPointerNull::get(types.get<void *>())};
    llvm::Value *py_symbol_table{func->getArg(0)};
    llvm::Value *stack_height_value{};
    size_t stack_height{};


    template <typename T>
    std::enable_if_t<std::is_integral_v<T>, llvm::Constant *>
    asValue(T t) {
        return llvm::ConstantInt::get(types.get<T>(), t);
    }

    // template <typename T>
    // std::enable_if_t<std::is_base_of_v<llvm::Value, std::remove_pointer_t<T>>, llvm::Value *>
    // asValue(llvm::Value *t) {
    //     return t;
    // }

    auto _createBlock(const llvm::Twine &name, llvm::Function *parent) {
        return llvm::BasicBlock::Create(context, name, parent);
    }

    auto createBlock(const char *name) {
        return _createBlock(useName(name), func);
    }

    auto createBlock(unsigned instr) {
        return _createBlock(useName("$instr.", instr), nullptr);
    }

    auto createBlock(unsigned instr, const char *extra) {
        return _createBlock(useName("$instr.", instr - 1, ".", extra), func);
    }

    template <typename T=char>
    auto getPointer(llvm::Value *base, size_t index, const llvm::Twine &name = "") {
        if (!index && name.isTriviallyEmpty()) {
            return base;
        }
        auto offset_value = asValue(sizeof(T) * index);
        return builder.CreateInBoundsGEP(types.get<char>(), base, offset_value, name);
    }

    template <typename T, typename M>
    auto getPointer(llvm::Value *instance, M T::* member, const llvm::Twine &name = "") {
        T dummy;
        auto offset = reinterpret_cast<const char *>(&(dummy.*member)) - reinterpret_cast<const char *>(&dummy);
        return getPointer(instance, offset, name);
    }

    template <typename T>
    auto readData(llvm::Value *base, size_t index, const llvm::Twine &name = "") {
        return builder.CreateLoad(types.get<T>(), getPointer<T>(base, index), name);
    }

    template <typename T, typename M>
    auto readData(llvm::Value *instance, M T::* member, const llvm::Twine &name = "") {
        return builder.CreateLoad(types.get<M>(), getPointer(instance, member), name);
    }

    template <typename T>
    auto writeData(llvm::Value *base, size_t index, llvm::Value *value) {
        return builder.CreateStore(value, getPointer<T>(base, index));
    }

    template <typename T, typename M>
    auto writeData(llvm::Value *instance, M T::* member, llvm::Value *value) {
        return builder.CreateStore(value, getPointer(instance, member));
    }

    void parseCFG();
    void emitBlock(unsigned index);
    llvm::Value *do_GETLOCAL(PyOparg oparg);
    void do_SETLOCAL(PyOparg oparg, llvm::Value *value, bool check_null = true);
    llvm::Value *do_POP();
    void do_PUSH(llvm::Value *value);
    void do_Py_INCREF(llvm::Value *v);
    void do_Py_DECREF(llvm::Value *v);

    template <typename T, typename M, typename ...Args>
    llvm::CallInst *do_CallSlot(llvm::Value *instance, M *T::* member, Args... args) {
        auto callee = readData(instance, member);
        return builder.CreateCall(types.get<M>(), callee, {args...});
    }

    template <ExternalSymbol S, typename ...Args>
    llvm::CallInst *do_CallSymbol(Args... args) {
        auto callee = readData<FunctionPointer>(py_symbol_table, S, symbol_names[S]);
        return builder.CreateCall(types.get<SymbolType<S>>(), callee, {args...});
    }

    void handle_UNARY_OP(ExternalSymbol s) {
        auto value = do_POP();
        auto callee = readData<FunctionPointer>(py_symbol_table, s, symbol_names[s]);
        using UnaryFunction = PyObject *(PyObject *);
        auto res = builder.CreateCall(types.get<UnaryFunction>(), callee, {value});
        do_Py_DECREF(value);
        do_PUSH(res);
    }

    void handle_BINARY_OP(ExternalSymbol s) {
        auto right = do_POP();
        auto left = do_POP();
        auto callee = readData<FunctionPointer>(py_symbol_table, s, symbol_names[s]);
        using BinaryFunction = PyObject *(PyObject *, PyObject *);
        auto res = builder.CreateCall(types.get<BinaryFunction>(), callee, {left, right});
        do_Py_DECREF(left);
        do_Py_DECREF(right);
        do_PUSH(res);
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

    llvm::BasicBlock *findBlock(unsigned instr_offset);


public:
    Translator() {
        func->setAttributes(
                llvm::AttributeList::get(context, llvm::AttributeList::FunctionIndex,
                        llvm::AttrBuilder(context)
                                .addAttribute(llvm::Attribute::NoUnwind)
                                .addAttribute("tune-cpu", llvm::sys::getHostCPUName())));
        py_symbol_table->setName(useName("$symbols"));
        func->getArg(1)->setName(useName("$locals"));
        func->getArg(2)->setName(useName("$locals"));
    }

    void *operator()(Compiler &compiler, PyCodeObject *code);
};

class PyInstrIterBase {
protected:
    const PyInstr *base;
    const size_t size;
public:
    size_t offset{0};
    PyOpcode opcode{};

    PyInstrIterBase(const PyInstr *base_, size_t size_) : base(base_), size(size_) {}
};


class QuickPyInstrIter : public PyInstrIterBase {
public:
    QuickPyInstrIter(const PyInstr *base_, size_t size_) : PyInstrIterBase(base_, size_) {}

    auto next() {
        if (offset == size) {
            return false;
        } else {
            opcode = _Py_OPCODE(base[offset]);
            offset++;
            return true;
        }
    }

    auto getOparg() {
        auto o = offset;
        auto oparg = _Py_OPARG(base[--o]);
        unsigned shift = 0;
        while (o && _Py_OPCODE(base[--o]) == EXTENDED_ARG) {
            shift += EXTENDED_ARG_BITS;
            oparg |= _Py_OPARG(base[o]) << shift;
        }
        return oparg;
    }
};

class PyInstrIter : public PyInstrIterBase {
    PyOparg ext_oparg{};
public:
    PyOparg oparg{};

    PyInstrIter(const PyInstr *base_, size_t size_) : PyInstrIterBase(base_, size_) {}

    auto next() {
        if (offset == size) {
            return false;
        } else {
            opcode = _Py_OPCODE(base[offset]);
            oparg = ext_oparg << EXTENDED_ARG_BITS | _Py_OPARG(base[offset]);
            offset++;
            ext_oparg = 0;
            return true;
        }
    }

    auto extend_current_oparg() {
        ext_oparg = oparg;
    }
};

#endif

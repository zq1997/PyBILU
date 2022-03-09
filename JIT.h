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


template<typename T, typename = void>
struct HasDereference : std::false_type {};
template<typename T>
struct HasDereference<T, std::void_t<decltype(*std::declval<T>())>> : std::true_type {};


template<typename Size=size_t, typename Iter=Size>
class Range {
    static_assert(std::is_integral<Size>::value);
    const Iter from;
    const Iter to;
public:
    class Iterator {
        Iter i;
    public:
        explicit Iterator(const Iter &i_) : i{i_} {}

        auto &operator++() {
            ++i;
            return *this;
        }

        auto operator!=(const Iterator &o) const { return o.i != i; }

        auto &operator*() {
            if constexpr(HasDereference<Iter>::value) {
                return *i;
            } else {
                return i;
            }
        }
    };

    Range(Size n, Iter from) : from{from}, to{from + n} {}

    explicit Range(Size n) : Range(n, Size{}) {}

    auto begin() { return Iterator{from}; }

    auto end() { return Iterator{to}; }
};

template<typename T>
class DynamicArray {
    std::unique_ptr<T[]> data;

public:
    using ValueType = T;

    DynamicArray() = default;

    explicit DynamicArray(size_t size, bool init = false) : data{init ? new T[size]{} : new T[size]} {};

    void reserve(size_t size, bool init = false) {
        data.reset(init ? new T[size]{} : new T[size]);
    }

    auto &operator[](size_t index) { return data[index]; }

    const auto &operator[](size_t index) const { return data[index]; }

    auto &operator*() const { return data[0]; }
};


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
    DynamicArray<unsigned> boundaries{};
    DynamicArray<llvm::BasicBlock *> ir_blocks{};

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

    [[nodiscard]] auto getOffset() const { return offset; }

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


class BitArray : public DynamicArray<unsigned long> {
public:
    static constexpr auto BitsPerValue = CHAR_BIT * sizeof(ValueType);

    static auto chunkNumber(size_t size) { return size / BitsPerValue + !!(size % BitsPerValue); };

    explicit BitArray(size_t size) : DynamicArray<ValueType>(chunkNumber(size)) {}

    void set(size_t index, bool value = true) {
        (*this)[index / BitsPerValue] |= ValueType{value} << index % BitsPerValue;
    }

    bool get(size_t index) {
        return (*this)[index / BitsPerValue] & (ValueType{1} << index % BitsPerValue);
    }

    [[nodiscard]] auto count(size_t size) const {
        size_t counted = 0;
        for (auto i = chunkNumber(size); i--;) {
            counted += std::bitset<BitsPerValue>((*this)[i]).count();
        }
        return counted;
    }
};

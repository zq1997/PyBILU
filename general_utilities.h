#ifndef PYNIC_GENERAL_UTILITIES
#define PYNIC_GENERAL_UTILITIES

#include <exception>
#include <string>
#include <type_traits>

#include <Python.h>
#include <opcode.h>

#ifdef NDEBUG
constexpr auto debug_build = false;
#define FOR_DEBUG(X)
#else
constexpr auto debug_build = true;
#define FOR_DEBUG(X) X
#endif

struct PyObjectRef {
    PyObject *o;

    PyObjectRef(const PyObjectRef &) = delete;

    PyObjectRef(PyObject *o) : o{o} {
        if (!o) {
            throw std::bad_exception();
        }
    }

    ~PyObjectRef() {
        Py_DECREF(o);
    }

    operator PyObject *() const { return o; }
};

inline const char *PyStringAsString(PyObject *o) {
    auto str = PyUnicode_AsUTF8(o);
    if (!str) {
        throw std::bad_exception();
    }
    return str;
}

inline PyObjectRef callDebugHelperFunction(const char *callee_name, const auto &... args) {
    PyObjectRef py_mod = PyImport_ImportModule("debug_helper");
    PyObjectRef py_callee = PyObject_GetAttrString(py_mod, callee_name);
    PyObject *py_args[]{nullptr, args...};
    return PyObject_Vectorcall(py_callee, &py_args[1], sizeof...(args) | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
}

template <typename T, auto DerefFunc>
class RangeIterator {
    T i;
public:
    explicit RangeIterator(T i) : i{i} {}

    auto &operator++() {
        ++i;
        return *this;
    }

    auto operator!=(const RangeIterator &o) const { return o.i != i; }

    auto &operator*() const { return DerefFunc(i); }
};

template <typename T1, typename T2 = T1>
class IntRange {
    using T = std::common_type_t<T1, T2>;
    static_assert(std::is_integral_v<T>);
    const T from;
    const T to;
    using Iterator = RangeIterator<T, [](auto &i) -> auto & { return i; }>;

public:
    IntRange(T1 from, T2 to) : from(from), to(to) {}

    explicit IntRange(T1 n) : from{0}, to{n} {}

    auto begin() { return Iterator{from}; }

    auto end() { return Iterator{to}; }
};

template <typename T>
class PtrRange {
    static_assert(std::is_pointer_v<T>);
    using Iterator = RangeIterator<T, [](auto &i) -> auto & { return *i; }>;
    const T from;
    const T to;
public:
    PtrRange(T base, size_t n) : from{base}, to{base + n} {}

    auto begin() { return Iterator{from}; }

    auto end() { return Iterator{to}; }
};

template <typename T>
class DynamicArray {
    T *data{};
    FOR_DEBUG(size_t array_size;)

public:
    DynamicArray() = default;

    DynamicArray(DynamicArray &&other) noexcept: data{other.data} {
        other.data = nullptr;
    };

    explicit DynamicArray(size_t size) { reserve(size); };

    ~DynamicArray() {
        if (data) {
            delete[] data;
        }
    };

    void reserve(size_t size) {
        assert(!data);
        data = new T[size];
        FOR_DEBUG(array_size = size;)
    }

    T &operator*() { return data[0]; }

    T &operator[](size_t index) {
        assert(index < array_size);
        return data[index];
    }

    const auto &operator[](size_t index) const { return data[index]; }
};


template <typename T>
class LimitedStack {
    DynamicArray<T> storage_space;
    T *stack_pointer;
public:
    LimitedStack() = default;

    void reserve(size_t size) {
        storage_space.reserve(size);
        stack_pointer = &storage_space[0];
    }

    auto empty() {
        return stack_pointer == &storage_space[0];
    }

    void push(const T &v) {
        *stack_pointer++ = v;
    }

    auto pop() {
        assert(!empty());
        return *--stack_pointer;
    }
};

class BitArray : public DynamicArray<uintmax_t> {
public:
    using ChunkType = uintmax_t;
    using Parent = DynamicArray<ChunkType>;

    static constexpr auto BitsPerValue = CHAR_BIT * sizeof(ChunkType);

    static auto chunkNumber(size_t size) { return size / BitsPerValue + !!(size % BitsPerValue); }

    BitArray() = default;

    explicit BitArray(size_t size, bool fill_with = false) : Parent(chunkNumber(size)) {
        fill(size, fill_with);
    }

    auto &getChunk(size_t index) { return Parent::operator[](index); }

    const auto &getChunk(size_t index) const { return Parent::operator[](index); }

    void reserve(size_t size, bool fill_with = false) {
        Parent::reserve(chunkNumber(size));
        fill(size, fill_with);
    }

    bool set(size_t index) {
        auto old = get(index);
        getChunk(index / BitsPerValue) |= ChunkType{1} << index % BitsPerValue;
        return !old;
    }

    void reset(size_t index) {
        getChunk(index / BitsPerValue) &= ~(ChunkType{1} << index % BitsPerValue);
    }

    void setIf(size_t index, bool cond) {
        getChunk(index / BitsPerValue) |= ChunkType{cond} << index % BitsPerValue;
    }

    bool get(size_t index) {
        return getChunk(index / BitsPerValue) & (ChunkType{1} << index % BitsPerValue);
    }

    void fill(size_t size, bool fill_with = false) {
        memset(&getChunk(0), fill_with ? UCHAR_MAX : 0, chunkNumber(size) * sizeof(ChunkType));
    }

    auto operator[](size_t index) = delete;
};

template <typename... Ts>
class BitArrayChunks {
    std::tuple<Ts &...> arrays;
    size_t chunk_num;

    class Iterator {
        BitArrayChunks &zip_arrays;
        size_t i;
    public:
        Iterator(BitArrayChunks &zip_arrays, size_t i) : zip_arrays(zip_arrays), i(i) {}

        auto &operator++() {
            ++i;
            return *this;
        }

        auto operator!=(const Iterator &o) const {
            assert(&zip_arrays == &o.zip_arrays);
            return o.i != i;
        }

        auto operator*() const {
            return std::apply([&](Ts &... x) { return std::tie(x.getChunk(i)...); }, zip_arrays.arrays);
        }

        explicit operator size_t() { return i; }
    };

public:
    BitArrayChunks(size_t size, Ts &... arrays) : arrays{arrays...}, chunk_num(BitArray::chunkNumber(size)) {}

    auto begin() { return Iterator{*this, 0}; }

    auto end() { return Iterator{*this, chunk_num}; }
};


class PyInstrPointer {
    _Py_CODEUNIT *pointer;

public:
    static constexpr auto extended_arg_shift = 8;

    explicit PyInstrPointer(PyCodeObject *py_code) :
            pointer(reinterpret_cast<_Py_CODEUNIT *>(PyBytes_AS_STRING(py_code->co_code))) {}

    explicit PyInstrPointer(_Py_CODEUNIT *pointer) : pointer(pointer) {}

    auto opcode() const { return _Py_OPCODE(*pointer); }

    auto rawOparg() const { return _Py_OPARG(*pointer); }

    auto oparg(const PyInstrPointer &instr_begin) const {
        auto p = pointer;
        auto arg = _Py_OPARG(*p);
        unsigned shift = 0;
        while (p-- != instr_begin.pointer && _Py_OPCODE(*p) == EXTENDED_ARG) {
            shift += extended_arg_shift;
            arg |= _Py_OPARG(*p) << shift;
        }
        return arg;
    }


    auto operator+(ptrdiff_t offset) const { return PyInstrPointer{pointer + offset}; }
};

// TODO: N-dim动态数组

#endif

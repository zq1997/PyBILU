#include "memory_manager.h"


using namespace std;
using namespace llvm::sys;


MemoryBlock loadCodeToMemory(const char *code, size_t size) {
    error_code ec;
    auto flag = Memory::ProtectionFlags::MF_RWE_MASK;
    auto mem = Memory::allocateMappedMemory(size, nullptr, flag, ec);
    if (ec) {
        // TODO: exception分为设置了PythonError的和没有的
        throw runtime_error(ec.message());
    }
    memcpy(mem.base(), code, size);
    return mem;
}

void unloadCode(MemoryBlock &mem) {
    Memory::releaseMappedMemory(mem);
}

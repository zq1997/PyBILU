#ifndef PYNIC_MEMORY_MANAGER_H
#define PYNIC_MEMORY_MANAGER_H

#include <cstring>

#include <llvm/Support/Memory.h>

llvm::sys::MemoryBlock loadCodeToMemory(const char *code, size_t size);

void unloadCode(llvm::sys::MemoryBlock &mem);

#endif

cmake_minimum_required(VERSION 3.20)
project(pynic)
set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "ON" FORCE)
set(CMAKE_CXX_STANDARD 17)

IF(CMAKE_BUILD_TYPE MATCHES Debug)
    find_package(LLVM 13 REQUIRED CONFIG PATHS /opt/llvm/build/compile-debug/lib/cmake/llvm/ NO_DEFAULT_PATH)
    # find_package(LLVM 13 REQUIRED CONFIG)
    list(APPEND llvm_libs LLVM-13)
    set(CPython_INCLUDE_DIRS "/opt/cpython/build/install-debug/include/python3.10d")
ELSE()
    find_package(LLVM 13 REQUIRED CONFIG PATHS /opt/llvm/build/compile-release/lib/cmake/llvm/ NO_DEFAULT_PATH)
    add_link_options(-s -Wl,--exclude-libs,ALL,--gc-sections)
    llvm_map_components_to_libnames(llvm_libs core native orcjit)
    set(CPython_INCLUDE_DIRS "/opt/cpython/build/install-release/include/python3.10")
ENDIF()

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS} ${CPython_INCLUDE_DIRS})
add_compile_options(-Wall -Wpedantic -fno-rtti)
link_directories(${LLVM_LIBRARY_DIRS})
link_libraries(${llvm_libs})

add_library(pynic SHARED main.cpp JIT.cpp)
set_target_properties(pynic PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        CXX_VISIBILITY_PRESET hidden
)

#target_precompile_headers(pynic PRIVATE "JIT.h")
#add_subdirectory(/opt/cpython cpython EXCLUDE_FROM_ALL)

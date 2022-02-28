cmake_minimum_required(VERSION 3.20)
project(PyBILU)
set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "ON" FORCE)
set(CMAKE_CXX_STANDARD 14)

IF(CMAKE_BUILD_TYPE MATCHES Debug)
    find_package(LLVM 13 REQUIRED CONFIG PATHS /opt/llvm/build/compile-debug/lib/cmake/llvm/ NO_DEFAULT_PATH)
    # find_package(LLVM 13 REQUIRED CONFIG)
    list(APPEND llvm_libs LLVM-13)
    set(CPython_INCLUDE_DIRS "/opt/cpython/build/install-debug/include/python3.10d")
ELSE()
    find_package(LLVM 13 REQUIRED CONFIG PATHS /opt/llvm/build/compile-release/lib/cmake/llvm/ NO_DEFAULT_PATH)
    add_link_options(-s -Wl,--exclude-libs,ALL,--gc-sections)
    llvm_map_components_to_libnames(llvm_libs native)
    set(CPython_INCLUDE_DIRS "/opt/cpython/build/install-release/include/python3.10")
ENDIF()

add_library(pybilu SHARED main.cpp JIT.cpp)
target_include_directories(pybilu PRIVATE ${LLVM_INCLUDE_DIRS} ${CPython_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(pybilu PRIVATE ${LLVM_DEFINITIONS_LIST})
target_compile_options(pybilu PRIVATE -fno-rtti)
#target_precompile_headers(pybilu PRIVATE "JIT.h")
target_link_directories(pybilu PRIVATE ${LLVM_LIBRARY_DIRS})
target_link_libraries(pybilu PRIVATE ${llvm_libs})
set_target_properties(pybilu PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        CXX_VISIBILITY_PRESET hidden
)

#add_subdirectory(/opt/cpython cpython EXCLUDE_FROM_ALL)

#add_library(ceval_impl_ ceval_impl.c)
#target_include_directories(ceval_impl_ PRIVATE ${CPython_INCLUDE_DIRS})
#add_custom_target(
#        ceval_impl
#        COMMAND clang -I${CPython_INCLUDE_DIRS} -emit-llvm
#            -S ${CMAKE_CURRENT_SOURCE_DIR}/ceval_impl.c -o ceval_impl.ll
#        SOURCES ceval_impl.c
#)
#add_dependencies(pybilu ceval_impl)


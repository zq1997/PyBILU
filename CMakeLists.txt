cmake_minimum_required(VERSION 3.20)
project(pynic)
set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "ON" FORCE)
set(CMAKE_CXX_STANDARD 17)

IF(CMAKE_BUILD_TYPE MATCHES Debug)
    find_package(LLVM 14 REQUIRED CONFIG PATHS /opt/llvm/build/compile-debug/lib/cmake/llvm/ NO_DEFAULT_PATH)
    set(CPython_INCLUDE_DIRS "/opt/cpython/build/install-debug/include/python3.10d")
ELSE()
    find_package(LLVM 14 REQUIRED CONFIG PATHS /opt/llvm/build/compile-release/lib/cmake/llvm/ NO_DEFAULT_PATH)
    set(CPython_INCLUDE_DIRS "/opt/cpython/build/install-release/include/python3.10")
ENDIF()

add_definitions(${LLVM_DEFINITIONS})
add_compile_definitions(Py_BUILD_CORE)
include_directories(${LLVM_INCLUDE_DIRS} ${CPython_INCLUDE_DIRS})
add_compile_options(-Wall -Wpedantic -Wno-unused-but-set-variable -Wno-c++20-attribute-extensions -fno-rtti)
link_directories(${LLVM_LIBRARY_DIRS})
add_link_options(-Wl,--exclude-libs,ALL,--gc-sections)

add_library(pynic SHARED main.cpp translator.cpp compiler.cpp shared_subroutines.cpp)
target_link_libraries(pynic LLVM-14)
set_target_properties(pynic PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        CXX_VISIBILITY_PRESET hidden
)

add_library(pynic_bundle SHARED $<TARGET_OBJECTS:pynic>)
llvm_map_components_to_libnames(llvm_libs core native passes)
target_link_libraries(pynic_bundle ${llvm_libs})
set_target_properties(pynic_bundle PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        CXX_VISIBILITY_PRESET hidden
)

add_subdirectory(test)
add_subdirectory(/opt/cpython cpython EXCLUDE_FROM_ALL)
#target_precompile_headers(pynic PRIVATE "JIT.h")

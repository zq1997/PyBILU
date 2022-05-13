cmake_minimum_required(VERSION 3.20)
project(pynic)
set(CMAKE_CXX_STANDARD 17)

IF(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CPython_INC_DIR_DEFAULT "/opt/cpython/build/install-debug/include/python3.10d")
    set(LLVM_DIR_DEFAULT "/opt/llvm/build/compile-debug/lib/cmake/llvm/")
ELSE()
    set(CPython_INC_DIR_DEFAULT "/opt/cpython/build/install-release/include/python3.10")
    set(LLVM_DIR_DEFAULT "/opt/llvm/build/compile-release/lib/cmake/llvm/")
ENDIF()

set(CPython_INC_DIR ${CPython_INC_DIR_DEFAULT} CACHE FILEPATH "CPython's include directory")
set(LLVM_DIR ${LLVM_DIR_DEFAULT} CACHE FILEPATH "LLVM 14 directory")
set(BUNDLE OFF CACHE BOOL "build a bundle")
message("Build type: CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
message("Use CPython at: CPython_INC_DIR=${CPython_INC_DIR}")
message("Use LLVM at: LLVM_DIR=${LLVM_DIR}")
message("Build a bundle: BUNDLE=${BUNDLE}")
find_package(LLVM 14 REQUIRED CONFIG PATHS ${LLVM_DIR} NO_DEFAULT_PATH)

add_definitions(${LLVM_DEFINITIONS})
add_compile_definitions(Py_BUILD_CORE)
include_directories(${LLVM_INCLUDE_DIRS} ${CPython_INC_DIR})
add_compile_options(-Wall -Wpedantic -Wno-unused-but-set-variable -fno-rtti -Wno-c++20-attribute-extensions)
link_directories(${LLVM_LIBRARY_DIRS})
add_link_options(-Wl,--exclude-libs,ALL,--gc-sections)

file(GLOB SRC CONFIGURE_DEPENDS "*.h" "*.cpp")
add_library(pynic SHARED ${SRC})
set_target_properties(pynic PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        CXX_VISIBILITY_PRESET hidden
)
IF(BUNDLE AND NOT CMAKE_BUILD_TYPE MATCHES Debug)
    llvm_map_components_to_libnames(llvm_libs core native passes)
    target_link_libraries(pynic ${llvm_libs})
ELSE()
    target_link_libraries(pynic LLVM-14)
ENDIF()

add_subdirectory(test EXCLUDE_FROM_ALL)
add_subdirectory(/opt/cpython cpython EXCLUDE_FROM_ALL)
# TODO: file(CREATE_LINK <original> <linkname> [...]) 来创建gdb_script的软链接

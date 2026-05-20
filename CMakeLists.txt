cmake_minimum_required(VERSION 3.10)
project(leanrndb VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_DEBUG "-g -O0")

set(LIBLINENOISE external/linenoise/linenoise.c)
set_source_files_properties(${LIBLINENOISE} PROPERTIES LANGUAGE C)

add_executable(leanrndb
    src/main.cpp
    src/pager.cpp
    src/btree.cpp
    src/tokenizer.cpp
    src/parser.cpp
    src/executor.cpp
    src/database.cpp
    ${LIBLINENOISE}
)
target_include_directories(leanrndb PRIVATE src external/linenoise)

if (NOT MSVC)
    target_link_libraries(leanrndb m)
endif()

enable_testing()
add_executable(test_basic
    tests/test_basic.cpp
    src/pager.cpp
    src/btree.cpp
    src/tokenizer.cpp
    src/parser.cpp
    src/executor.cpp
    src/database.cpp
)
target_include_directories(test_basic PRIVATE src external/linenoise)

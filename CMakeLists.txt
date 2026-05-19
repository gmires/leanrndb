cmake_minimum_required(VERSION 3.10)
project(leanrndb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(leanrndb
    src/main.cpp
    src/pager.cpp
    src/btree.cpp
    src/tokenizer.cpp
    src/parser.cpp
    src/executor.cpp
    src/database.cpp
)
target_include_directories(leanrndb PRIVATE src)

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
target_include_directories(test_basic PRIVATE src)

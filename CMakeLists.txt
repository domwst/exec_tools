cmake_minimum_required(VERSION 3.5)
project(exec_tools)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "bin")

include(cmake/CompilerCheck.cmake)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -g -fno-omit-frame-pointer)
add_subdirectory(third_party)

add_subdirectory(common)

add_subdirectory(clang_compile)
add_subdirectory(run)

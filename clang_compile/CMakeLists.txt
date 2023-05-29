file(GLOB_RECURSE CLANG_COMPILE_SOURCES "./*.cpp")

add_executable(clang_compile ${CLANG_COMPILE_SOURCES})
target_link_libraries(clang_compile PUBLIC sex common_lib)

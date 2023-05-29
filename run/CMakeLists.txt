file(GLOB_RECURSE CLANG_COMPILE_SOURCES "./*.cpp")

add_executable(run ${CLANG_COMPILE_SOURCES})
target_link_libraries(run PUBLIC sex common_lib)

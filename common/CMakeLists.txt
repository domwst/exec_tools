cmake_minimum_required(VERSION 3.5)

get_filename_component(LIB_INCLUDE_PATH ".." ABSOLUTE)
get_filename_component(LIB_PATH "." ABSOLUTE)

file(GLOB_RECURSE LIB_CXX_SOURCES ${LIB_PATH}/*.cpp)
file(GLOB_RECURSE LIB_HEADERS ${LIB_PATH}/*.hpp)

add_library(common_lib STATIC ${LIB_CXX_SOURCES} ${LIB_HEADERS})
target_include_directories(common_lib PUBLIC ${LIB_INCLUDE_PATH})
target_link_libraries(common_lib PUBLIC sex)

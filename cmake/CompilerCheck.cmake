set(REQUIRED_CXX_COMPILER "Clang")
set(MIN_CXX_COMPITER_VERSION 14.0)

message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL REQUIRED_CXX_COMPILER)
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS MIN_CXX_COMPITER_VERSION)
        message(
                FATAL_ERROR
                "Old version of ${REQUIRED_CXX_COMPILER}: ${CMAKE_CXX_COMPILER_VERSION}, required ${MIN_CXX_COMPITER_VERSION}"
        )
    endif()
else()
    message(
            FATAL_ERROR
            "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Use ${REQUIRED_CXX_COMPILER}, version >= ${MIN_CXX_COMPITER_VERSION}"
    )
endif()

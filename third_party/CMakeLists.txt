cmake_minimum_required(VERSION 3.5)

include(FetchContent)

# --------------------------------------------------------------------

# set(FETCHCONTENT_FULLY_DISCONNECTED ON)
# set(FETCHCONTENT_QUIET OFF)

# --------------------------------------------------------------------

FetchContent_Declare(
        sex
        GIT_REPOSITORY https://github.com/domwst/sex.git
        GIT_TAG master
)
FetchContent_MakeAvailable(sex)

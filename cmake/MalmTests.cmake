# Fetch doctest and provide malm_add_test().
include(FetchContent)

set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
    GIT_SHALLOW ON
    SYSTEM)
FetchContent_MakeAvailable(doctest)
list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
include(doctest)

# malm_add_test(NAME <name> LABEL <fast|slow> SOURCES <files...> [LIBS <targets...>] [PREFIX <text>]
#                  [TIMEOUT <seconds>])
# Builds one executable (with the shared doctest main TU) and registers its test cases. PREFIX is
# prepended to the ctest names; needed when two executables share test case names (one source built
# twice with different sizes and labels). TIMEOUT sets the ctest timeout of every test case.
function(malm_add_test)
    cmake_parse_arguments(ARG "" "NAME;LABEL;PREFIX;TIMEOUT" "SOURCES;LIBS" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_SOURCES)
        message(FATAL_ERROR "malm_add_test: NAME and SOURCES are required")
    endif()
    if(NOT ARG_LABEL MATCHES "^(fast|slow)$")
        message(FATAL_ERROR "malm_add_test: LABEL must be fast or slow")
    endif()
    add_executable(${ARG_NAME} "${PROJECT_SOURCE_DIR}/tests/doctest_main.cpp" ${ARG_SOURCES})
    target_link_libraries(${ARG_NAME} PRIVATE malm_warnings doctest::doctest ${ARG_LIBS})
    set(props LABELS ${ARG_LABEL})
    if(ARG_TIMEOUT)
        list(APPEND props TIMEOUT ${ARG_TIMEOUT})
    endif()
    doctest_discover_tests(${ARG_NAME} TEST_PREFIX "${ARG_PREFIX}" PROPERTIES ${props})
endfunction()

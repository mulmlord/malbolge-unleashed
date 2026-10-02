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
#
# Per-case slow marking: with LABEL fast, a test case tagged `* doctest::test_suite("slow")` in the
# sources is registered under the label slow (ctest name prefixed "slow:") and left out of fast, so
# a heavy case can sit next to the cheap ones it belongs with. Each executable is discovered twice
# (doctest hashes TEST_SPEC into the generated include file, so the two registrations coexist); the
# second one only if a source mentions the slow suite. With LABEL slow every case is slow.
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
    # doctest_discover_tests() puts TEST_SPEC behind "--test-case=" and then splits it as a list, so
    # "*;--flag" reaches the executable as two arguments, "--test-case=*" and "--flag". Pinned to
    # doctest v2.5.3 above; a doctest upgrade must keep this working (see the case accounting in
    # the CI notes: `ctest -N -L fast` plus `-L slow` must list every case exactly once).
    set(fast_spec "*" "--test-suite-exclude=slow")
    set(slow_spec "*" "--test-suite=slow")
    if(NOT ARG_LABEL STREQUAL "fast")
        doctest_discover_tests(${ARG_NAME} TEST_PREFIX "${ARG_PREFIX}" PROPERTIES ${props})
        return()
    endif()
    doctest_discover_tests(${ARG_NAME} TEST_PREFIX "${ARG_PREFIX}"
        TEST_SPEC ${fast_spec} PROPERTIES ${props})
    set(has_slow FALSE)
    foreach(src IN LISTS ARG_SOURCES)
        cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE path)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        file(READ "${path}" text)
        if(text MATCHES "test_suite\\(\"slow\"\\)")
            set(has_slow TRUE)
        endif()
    endforeach()
    if(has_slow)
        set(slow_props ${props})
        list(TRANSFORM slow_props REPLACE "^fast$" "slow")
        doctest_discover_tests(${ARG_NAME} TEST_PREFIX "slow:${ARG_PREFIX}"
            TEST_SPEC ${slow_spec} PROPERTIES ${slow_props})
    endif()
endfunction()

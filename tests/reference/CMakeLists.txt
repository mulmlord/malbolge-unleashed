# Differential test against Olmstead's reference interpreter (see diff_test.cpp).
# The reference is built as C, which the top-level project does not enable.
enable_language(C)

set(MALM_REF_PRISTINE "${CMAKE_CURRENT_SOURCE_DIR}/olmstead/malbolge.c")
set(MALM_REF_INSTRUMENTED "${CMAKE_CURRENT_BINARY_DIR}/malbolge_instrumented.c")

# Instrumented copy of the pristine reference, produced at build time (instrument.cmake aborts
# the build if any expected snippet is missing).
add_custom_command(
    OUTPUT "${MALM_REF_INSTRUMENTED}"
    COMMAND ${CMAKE_COMMAND} -DIN=${MALM_REF_PRISTINE} -DOUT=${MALM_REF_INSTRUMENTED}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/instrument.cmake"
    DEPENDS "${MALM_REF_PRISTINE}" "${CMAKE_CURRENT_SOURCE_DIR}/instrument.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/reference.h"
    COMMENT "Instrumenting the reference malbolge.c"
    VERBATIM)

# Third-party C: deliberately without malm_warnings. The sanitizers still apply to it.
add_library(malm_reference STATIC "${MALM_REF_INSTRUMENTED}")
target_include_directories(malm_reference PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(MALM_SANITIZE)
    target_compile_options(malm_reference PRIVATE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
endif()

# One source, two sizes: ~100 programs x 2,000 steps in the fast label; the issue's 1,000 programs
# x 10,000 steps nightly in the slow label, plus beer99 (13.8 million steps) in lockstep.
malm_add_test(NAME reference_diff_test LABEL fast SOURCES diff_test.cpp
              LIBS malm_core malm_reference TIMEOUT 120)
target_compile_definitions(reference_diff_test PRIVATE MALM_DIFF_PROGRAMS=100 MALM_DIFF_STEPS=2000)

malm_add_test(NAME reference_diff_slow_test LABEL slow SOURCES diff_test.cpp
              LIBS malm_core malm_reference PREFIX "slow:" TIMEOUT 600)
target_compile_definitions(reference_diff_slow_test PRIVATE
    MALM_DIFF_PROGRAMS=1000 MALM_DIFF_STEPS=10000
    MALM_DIFF_BEER99="${PROJECT_SOURCE_DIR}/tests/programs/beer99.mal")

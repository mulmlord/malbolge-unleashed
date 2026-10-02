include(${PROJECT_SOURCE_DIR}/cmake/MalmTests.cmake)

malm_add_test(NAME cli_test LABEL fast
    SOURCES driver/cli_test.cpp
    LIBS malm_driver)

malm_add_test(NAME trailer_test LABEL fast SOURCES pack/trailer_test.cpp LIBS malm_pack)

malm_add_test(NAME value_test LABEL fast
    SOURCES core/value_test.cpp
    LIBS malm_core)
malm_add_test(NAME tables_test LABEL fast SOURCES core/tables_test.cpp LIBS malm_core)
malm_add_test(NAME ops_test LABEL fast SOURCES core/ops_test.cpp LIBS malm_core)
malm_add_test(NAME memory_test LABEL fast SOURCES core/memory_test.cpp LIBS malm_core)
malm_add_test(NAME loader_test LABEL fast SOURCES core/loader_test.cpp LIBS malm_core)
malm_add_test(NAME classic_test LABEL fast SOURCES core/classic_test.cpp LIBS malm_core)
malm_add_test(NAME vm_test LABEL fast SOURCES core/vm_test.cpp LIBS malm_core)

malm_add_test(NAME container_test LABEL fast SOURCES container/container_test.cpp
              LIBS malm_container libzstd_static TIMEOUT 120)
target_compile_definitions(container_test PRIVATE
    MALM_PROGRAMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/programs"
    MALM_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")

malm_add_test(NAME debug_test LABEL fast SOURCES driver/debug_test.cpp LIBS malm_driver TIMEOUT 300)
target_compile_definitions(debug_test PRIVATE
    MALM_PROGRAMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/programs"
    MALM_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
# The same source again with only the 13.8M-step classic beer99 stats case (slow under ASan).
malm_add_test(NAME debug_slow_test LABEL slow SOURCES driver/debug_test.cpp LIBS malm_driver
              PREFIX "slow:" TIMEOUT 600)
target_compile_definitions(debug_slow_test PRIVATE MALM_SLOW_CASES
    MALM_PROGRAMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/programs"
    MALM_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")

# CLI tests against the committed programs (see tests/programs/SOURCES.txt).
target_compile_definitions(cli_test PRIVATE MALM_PROGRAMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/programs")

foreach(case halting_cat loop_cat epipe debug_halting_cat debug_hello debug_epipe debug_trace_flush debug_stderr_dead hello classic_beer99 classic_loop_cat
             malm_hello malm_beer99 malm_loop_cat malm_epipe malm_text_pipe
             pack_cat pack_classic_cat pack_hello pack_beer99 pack_exit_a pack_ignores_args
             pack_repro pack_corrupt pack_epipe pack_replace pack_closed_stdin pack_closed_stdout pack_efbig)
    add_test(NAME pipe_${case}
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/driver/pipe_test.sh
                $<TARGET_FILE:malm> ${CMAKE_CURRENT_SOURCE_DIR}/programs ${case})
    set_tests_properties(pipe_${case} PROPERTIES LABELS fast TIMEOUT 60)
endforeach()
add_subdirectory(corpus)
add_subdirectory(reference)

malm_add_test(NAME cycles_test LABEL fast SOURCES asm/cycles_test.cpp LIBS malm_asm)

include(${PROJECT_SOURCE_DIR}/cmake/MalmTests.cmake)

malm_add_test(NAME cli_test LABEL fast
    SOURCES driver/cli_test.cpp
    LIBS malm_driver)

malm_add_test(NAME value_test LABEL fast
    SOURCES core/value_test.cpp
    LIBS malm_core)
malm_add_test(NAME tables_test LABEL fast SOURCES core/tables_test.cpp LIBS malm_core)
malm_add_test(NAME ops_test LABEL fast SOURCES core/ops_test.cpp LIBS malm_core)
malm_add_test(NAME memory_test LABEL fast SOURCES core/memory_test.cpp LIBS malm_core)
malm_add_test(NAME loader_test LABEL fast SOURCES core/loader_test.cpp LIBS malm_core)
malm_add_test(NAME vm_test LABEL fast SOURCES core/vm_test.cpp LIBS malm_core)

# CLI tests against the committed programs (see tests/programs/SOURCES.txt).
target_compile_definitions(cli_test PRIVATE MALM_PROGRAMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/programs")

foreach(case halting_cat loop_cat epipe hello)
    add_test(NAME pipe_${case}
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/driver/pipe_test.sh
                $<TARGET_FILE:malm> ${CMAKE_CURRENT_SOURCE_DIR}/programs ${case})
    set_tests_properties(pipe_${case} PROPERTIES LABELS fast TIMEOUT 60)
endforeach()

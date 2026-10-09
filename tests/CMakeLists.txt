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

malm_add_test(NAME debug_test LABEL fast SOURCES driver/debug_test.cpp LIBS malm_driver TIMEOUT 600)
target_compile_definitions(debug_test PRIVATE
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
    if(case MATCHES "^(malm_beer99|pack_beer99)$")
        set(pipe_label slow) # the same song as pipe_classic_beer99, through other front ends
    else()
        set(pipe_label fast)
    endif()
    set_tests_properties(pipe_${case} PROPERTIES LABELS ${pipe_label} TIMEOUT 60)
endforeach()
add_subdirectory(corpus)
add_subdirectory(reference)

malm_add_test(NAME cycles_test LABEL fast SOURCES asm/cycles_test.cpp LIBS malm_asm)

malm_add_test(NAME asm_front_test LABEL fast
    SOURCES asm/parser_test.cpp asm/limits_test.cpp asm/errors_test.cpp asm/roundtrip_test.cpp asm/fuzz_test.cpp
    LIBS malm_asm TIMEOUT 600)
target_compile_definitions(asm_front_test PRIVATE MALM_ASM_DOC="${PROJECT_SOURCE_DIR}/docs/asm.md")

malm_add_test(NAME asm_layout_test LABEL fast
    SOURCES asm/layout_test.cpp asm/units_test.cpp asm/startup_test.cpp
    LIBS malm_asm)

# The golden examples of examples/asm (issue #16): in-process, and through the CLI with real pipes.
malm_add_test(NAME asm_examples_test LABEL fast SOURCES asm/examples_test.cpp
    LIBS malm_asm malm_container TIMEOUT 300)
target_compile_definitions(asm_examples_test PRIVATE
    MALM_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples/asm")
# The disassembler (issue #17): the golden examples round-trip through `malm disasm`.
malm_add_test(NAME asm_disasm_test LABEL fast SOURCES asm/disasm_test.cpp
    LIBS malm_asm TIMEOUT 300)
target_compile_definitions(asm_disasm_test PRIVATE
    MALM_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples/asm")
foreach(case hello cat halting_cat digital_root adder)
    add_test(NAME asm_example_${case}
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/asm/examples_pipe_test.sh
                $<TARGET_FILE:malm> ${PROJECT_SOURCE_DIR}/examples/asm ${case})
    set_tests_properties(asm_example_${case} PROPERTIES LABELS fast TIMEOUT 120)
endforeach()

# The high-level assembler (issue #19).
malm_add_test(NAME hla_word_test LABEL fast SOURCES hla/word_test.cpp LIBS malm_hla)
malm_add_test(NAME hla_lang_test LABEL fast SOURCES hla/lang_test.cpp LIBS malm_hla TIMEOUT 600)
# Calls, the stack, switch, computed jumps and RAM (issue #20).
malm_add_test(NAME hla_control_test LABEL fast SOURCES hla/control_test.cpp LIBS malm_hla
    TIMEOUT 600)
malm_add_test(NAME hla_ram_test LABEL fast SOURCES hla/ram_test.cpp LIBS malm_hla TIMEOUT 600)
malm_add_test(NAME hla_examples_test LABEL fast SOURCES hla/examples_test.cpp
    LIBS malm_hla TIMEOUT 600)
target_compile_definitions(hla_examples_test PRIVATE
    MALM_HLA_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples/hla"
    MALM_ASM_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples/asm")
foreach(case hello cat exit42 rev errors)
    add_test(NAME hla_example_${case}
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/hla/examples_pipe_test.sh
                $<TARGET_FILE:malm> ${PROJECT_SOURCE_DIR}/examples/hla
                ${PROJECT_SOURCE_DIR}/examples/asm ${case})
    set_tests_properties(hla_example_${case} PROPERTIES LABELS fast TIMEOUT 300)
endforeach()

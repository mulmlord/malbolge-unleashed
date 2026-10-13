configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp" @ONLY)

# The RISC-V runtime as byte arrays (runtime_files.hpp), for `malm build` of C sources.
set(rt_dir "${PROJECT_SOURCE_DIR}/rv/runtime")
file(GLOB rt_files CONFIGURE_DEPENDS "${rt_dir}/include/*.h" "${rt_dir}/src/*.c")
set(rt_embed "${CMAKE_CURRENT_BINARY_DIR}/generated/runtime_files.cpp")
add_custom_command(OUTPUT "${rt_embed}"
    COMMAND "${CMAKE_COMMAND}" -DRUNTIME_DIR=${rt_dir} -DOUTPUT=${rt_embed}
        -P "${PROJECT_SOURCE_DIR}/cmake/EmbedRuntime.cmake"
    DEPENDS "${PROJECT_SOURCE_DIR}/cmake/EmbedRuntime.cmake" "${rt_dir}/crt0.S" "${rt_dir}/link.ld"
        ${rt_files}
    COMMENT "Embedding the RISC-V runtime"
    VERBATIM)

add_library(malm_driver asm_cmd.cpp build_cmd.cpp hla_cmd.cpp cli.cpp disasm_cmd.cpp run_cmd.cpp debug_observer.cpp export_cmd.cpp pack_cmd.cpp packed_run.cpp program_io.cpp rvref_cmd.cpp rvemu_cmd.cpp rvaot_cmd.cpp toolchain.cpp "${rt_embed}")
target_include_directories(malm_driver
    PUBLIC "${PROJECT_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(malm_driver PUBLIC malm_core malm_asm malm_hla malm_container malm_pack malm_rv malm_aot PRIVATE malm_warnings)

if(MALM_STATIC)
    target_compile_definitions(malm_driver PUBLIC MALM_STATIC)
endif()

add_executable(malm main.cpp)
target_link_libraries(malm PRIVATE malm_driver malm_warnings)
if(MALM_STATIC)
    target_link_options(malm PRIVATE -static)
endif()

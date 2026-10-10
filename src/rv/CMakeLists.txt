# RISC-V support (docs/rv.md): the RV32IM ELF loader and the C++ reference emulator.
add_library(malm_rv STATIC elf.cpp machine.cpp)
target_include_directories(malm_rv PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(malm_rv PUBLIC malm_core PRIVATE malm_warnings)

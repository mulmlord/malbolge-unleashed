# RISC-V support (docs/rv.md): the RV32IM ELF loader, the C++ reference emulator and the generator
# of rv-emu's HLA program with the instruction semantics it shares with rv-aot.
add_library(malm_rv STATIC elf.cpp machine.cpp semantics.cpp emu.cpp)
target_include_directories(malm_rv PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(malm_rv PUBLIC malm_core PRIVATE malm_warnings)

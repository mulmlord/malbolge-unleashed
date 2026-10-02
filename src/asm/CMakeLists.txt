# Header-only: constexpr per-address cycle/placement tables for the assembler.
add_library(malm_asm INTERFACE)
target_include_directories(malm_asm INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(malm_asm INTERFACE malm_core)

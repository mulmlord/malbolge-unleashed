# The high-level assembler (docs/hla.md): parser, word representation and lowering to the
# assembler's AST.
add_library(malm_hla STATIC parser.cpp word.cpp lower.cpp)
target_include_directories(malm_hla PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(malm_hla PUBLIC malm_asm malm_core PRIVATE malm_warnings)

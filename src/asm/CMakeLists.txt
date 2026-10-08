# Front end of the assembler (lexer, parser, AST, static checks, printer); cycles.hpp holds the
# header-only constexpr per-address cycle/placement tables.
add_library(malm_asm STATIC diagnostics.cpp ast.cpp lexer.cpp parser.cpp check.cpp printer.cpp)
target_include_directories(malm_asm PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(malm_asm PUBLIC malm_core PRIVATE malm_warnings)

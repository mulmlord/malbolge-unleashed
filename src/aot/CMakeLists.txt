# rv-aot (docs/rv.md 9): RV32IM programs translated to HLA, with rv-emu's instruction semantics.
add_library(malm_aot STATIC cfg.cpp aot.cpp)
target_include_directories(malm_aot PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(malm_aot PUBLIC malm_rv malm_hla PRIVATE malm_warnings)

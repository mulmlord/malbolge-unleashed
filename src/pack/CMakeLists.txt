add_library(malm_pack STATIC trailer.cpp)
target_include_directories(malm_pack PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(malm_pack PUBLIC malm_core PRIVATE malm_warnings)

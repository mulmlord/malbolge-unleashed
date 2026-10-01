add_library(malm_core STATIC value.cpp memory.cpp)
target_include_directories(malm_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(malm_core PRIVATE malm_warnings)

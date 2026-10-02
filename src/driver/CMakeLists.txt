configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp" @ONLY)

add_library(malm_driver cli.cpp run_cmd.cpp export_cmd.cpp pack_cmd.cpp packed_run.cpp program_io.cpp)
target_include_directories(malm_driver
    PUBLIC "${PROJECT_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(malm_driver PUBLIC malm_core malm_container malm_pack PRIVATE malm_warnings)

if(MALM_STATIC)
    target_compile_definitions(malm_driver PUBLIC MALM_STATIC)
endif()

add_executable(malm main.cpp)
target_link_libraries(malm PRIVATE malm_driver malm_warnings)
if(MALM_STATIC)
    target_link_options(malm PRIVATE -static)
endif()

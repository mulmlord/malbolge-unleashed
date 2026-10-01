configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp" @ONLY)

add_library(malm_driver cli.cpp)
target_include_directories(malm_driver
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(malm_driver PRIVATE malm_warnings)

add_executable(malm main.cpp)
target_link_libraries(malm PRIVATE malm_driver malm_warnings)
if(MALM_STATIC)
    target_link_options(malm PRIVATE -static)
endif()

include(${PROJECT_SOURCE_DIR}/cmake/MalmTests.cmake)

malm_add_test(NAME cli_test LABEL fast
    SOURCES driver/cli_test.cpp
    LIBS malm_driver)

malm_add_test(NAME value_test LABEL fast
    SOURCES core/value_test.cpp
    LIBS malm_core)

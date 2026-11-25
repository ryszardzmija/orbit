add_executable(config_tests
    config_test.cpp
)

target_link_libraries(config_tests PRIVATE
    config
    GTest::gtest_main
)

orbit_discover_unit_tests(config_tests)

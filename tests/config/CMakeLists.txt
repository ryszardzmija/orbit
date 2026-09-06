add_executable(config_tests
    config_test.cpp
)

target_link_libraries(config_tests PRIVATE
    config
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(config_tests)

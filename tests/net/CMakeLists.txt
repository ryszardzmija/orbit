add_executable(net_tests
    address_format_test.cpp
)

target_link_libraries(net_tests PRIVATE
    net
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(net_tests)

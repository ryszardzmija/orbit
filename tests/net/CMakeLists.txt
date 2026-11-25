add_executable(net_tests
    address_format_test.cpp
)

target_link_libraries(net_tests PRIVATE
    net
    GTest::gtest_main
)

orbit_discover_unit_tests(net_tests)

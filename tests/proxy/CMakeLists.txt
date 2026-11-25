add_executable(proxy_tests
    send_buffer_test.cpp
    session_manager_test.cpp
)

target_link_libraries(proxy_tests PRIVATE
    proxy
    GTest::gtest_main
)

orbit_discover_unit_tests(proxy_tests)

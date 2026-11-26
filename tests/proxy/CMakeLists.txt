add_executable(proxy_tests
    shutdown_signal_test.cpp
    send_buffer_test.cpp
    session_manager_test.cpp
    reactor_test.cpp
    resource_recovery_test.cpp
    global_buffer_test.cpp
    idle_session_test.cpp
    deadlines_test.cpp
    upstream_dialer_test.cpp
)

target_link_libraries(proxy_tests PRIVATE
    proxy
    GTest::gtest_main
)

orbit_discover_unit_tests(proxy_tests)

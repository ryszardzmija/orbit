add_executable(proxy_tests
    shutdown_signal_test.cpp
    send_buffer_test.cpp
    session_manager_test.cpp
    reactor_test.cpp
    controlled_clock.cpp
    deadlines_test.cpp
    upstream_dialer_test.cpp
)

target_link_libraries(proxy_tests PRIVATE
    proxy
    GTest::gtest_main
)

# Wrap the libstdc++ steady clock and socket calls in the test binary only.
target_link_options(proxy_tests PRIVATE
    "LINKER:--wrap=_ZNSt6chrono3_V212steady_clock3nowEv"
    "LINKER:--wrap=socket"
)

orbit_discover_unit_tests(proxy_tests)

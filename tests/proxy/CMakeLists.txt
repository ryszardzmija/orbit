add_executable(proxy_tests
    send_buffer_test.cpp    
)

target_link_libraries(proxy_tests PRIVATE
    proxy
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(proxy_tests)

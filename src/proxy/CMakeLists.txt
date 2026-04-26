add_library(proxy
    epoll_utils.cpp
    forwarding.cpp
    pending_data_sender.cpp
    proxy.cpp
    send_buffer_factory.cpp
    send_buffer.cpp
    session_pair.cpp
    socket_io.cpp
)

target_include_directories(proxy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(proxy PRIVATE common spdlog::spdlog)

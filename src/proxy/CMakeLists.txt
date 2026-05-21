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

target_include_directories(proxy PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(proxy
    PUBLIC
        absl::flat_hash_map
    PRIVATE
        common
        spdlog::spdlog
)

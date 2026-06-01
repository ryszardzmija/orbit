add_library(proxy
    proxy.cpp
    detail/forwarding.cpp
    detail/pending_data_sender.cpp
    detail/send_buffer_factory.cpp
    detail/send_buffer.cpp
    detail/session_pair.cpp
    detail/signal_fd.cpp
    detail/timer_fd.cpp
    detail/resolved_upstream.cpp
    detail/upstream_dialer.cpp
    detail/epoll/epoll_poller.cpp
    detail/sources/reactor_sources.cpp
)

target_include_directories(proxy PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(proxy
    PUBLIC
        absl::flat_hash_map
    PRIVATE
        common
        spdlog::spdlog
        net
        signal
)

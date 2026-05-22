add_library(net
    resolver.cpp
    dialer.cpp
    address_format.cpp
    socket_options.cpp
    listener.cpp
    socket_io.cpp
)

target_link_libraries(net PUBLIC common)

add_library(net
    resolver.cpp
    dialer.cpp
    address_format.cpp
    socket_options.cpp
)

target_link_libraries(net PRIVATE common)

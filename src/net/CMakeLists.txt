add_library(net
    resolver.cpp
)

target_link_libraries(net PRIVATE common)

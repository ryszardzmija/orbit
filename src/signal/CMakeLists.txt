add_library(signal
    signal_block.cpp
)

target_include_directories(signal PUBLIC ${CMAKE_SOURCE_DIR}/src)

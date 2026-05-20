add_library(logging
    logger.cpp
)

target_link_libraries(logging PUBLIC spdlog::spdlog)

target_include_directories(logging PUBLIC ${CMAKE_SOURCE_DIR}/src)

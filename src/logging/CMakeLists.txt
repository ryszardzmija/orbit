add_library(logging
    logger.cpp
)

target_link_libraries(logging PUBLIC spdlog::spdlog)

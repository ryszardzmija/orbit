cmake_minimum_required(VERSION 3.28)

project(orbit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.6.2
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(cli11)

add_subdirectory(src/proxy)
add_subdirectory(src/config)
add_subdirectory(src/logging)
add_subdirectory(src/net)

add_executable(orbit
    src/main.cpp
    src/socket_utils.cpp
    src/address_utils.cpp
)

target_include_directories(orbit PRIVATE src)
target_link_libraries(orbit PRIVATE proxy config logging net spdlog::spdlog CLI11::CLI11)
target_compile_options(orbit PRIVATE -Wall -Wextra -Wpedantic)

option(ORBIT_BUILD_TESTS "Build Unit Tests" OFF)

if (ORBIT_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
    )
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(tests)
endif()

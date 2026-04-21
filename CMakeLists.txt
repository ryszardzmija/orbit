cmake_minimum_required(VERSION 3.28)

project(orbit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(src/proxy)

add_executable(orbit
    src/main.cpp
    src/socket_utils.cpp
    src/address_utils.cpp
)

target_include_directories(orbit PRIVATE src)
target_link_libraries(orbit PRIVATE proxy)
target_compile_options(orbit PRIVATE -Wall -Wextra -Wpedantic)

option(ORBIT_BUILD_TESTS "Build Unit Tests" OFF)

if (ORBIT_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
    )
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(tests)
endif()

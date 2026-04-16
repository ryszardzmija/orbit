cmake_minimum_required(VERSION 3.28)

project(orbit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(orbit
    src/main.cpp
    src/socket_utils.cpp
    src/address_utils.cpp
)

target_compile_options(orbit PRIVATE -Wall -Wextra -Wpedantic)

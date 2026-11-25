cmake_minimum_required(VERSION 3.28)

project(orbit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ORBIT_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if (ORBIT_ENABLE_SANITIZERS)
    include(cmake/Sanitizers.cmake)
endif()

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

set(ABSL_PROPAGATE_CXX_STD ON)

FetchContent_Declare(
    abseil
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG 20260107.1
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
block(SCOPE_FOR VARIABLES)
    if (ORBIT_ENABLE_SANITIZERS)
        # Abseil's compiled hash-table code and Orbit must agree on the ASan-dependent layout.
        string(JOIN " " abseil_compile_flags ${orbit_sanitizer_compile_flags})
        string(JOIN " " abseil_link_flags ${orbit_sanitizer_link_flags})
        string(APPEND CMAKE_CXX_FLAGS " ${abseil_compile_flags}")
        string(APPEND CMAKE_SHARED_LINKER_FLAGS " ${abseil_link_flags}")
    endif()
    FetchContent_MakeAvailable(abseil)
endblock()

add_subdirectory(src/proxy)
add_subdirectory(src/config)
add_subdirectory(src/logging)
add_subdirectory(src/net)
add_subdirectory(src/common)
add_subdirectory(src/signal)

add_executable(orbit
    src/main.cpp
)

target_include_directories(orbit PRIVATE src)
target_link_libraries(orbit PRIVATE proxy config logging net common signal spdlog::spdlog CLI11::CLI11)
target_compile_options(orbit PRIVATE -Wall -Wextra -Wpedantic)

if (ORBIT_ENABLE_SANITIZERS)
    foreach(target IN ITEMS orbit proxy config logging net signal)
        target_link_libraries(${target} PRIVATE orbit_sanitizers)
    endforeach()
endif()

option(ORBIT_BUILD_TESTS "Build and register Orbit tests" OFF)

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

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

set(orbit_sanitizer_link_flags -fsanitize=address,undefined)
set(orbit_sanitizer_compile_flags
    ${orbit_sanitizer_link_flags}
    -fno-sanitize-recover=all
    -fsanitize-address-use-after-scope
    -fno-omit-frame-pointer
    -g
)

cmake_push_check_state(RESET)
string(JOIN " " CMAKE_REQUIRED_FLAGS ${orbit_sanitizer_compile_flags})
set(CMAKE_REQUIRED_LINK_OPTIONS ${orbit_sanitizer_link_flags})
check_cxx_source_compiles("int main() { return 0; }" ORBIT_SANITIZER_FLAGS_SUPPORTED)
cmake_pop_check_state()

if (NOT ORBIT_SANITIZER_FLAGS_SUPPORTED)
    message(FATAL_ERROR "ORBIT_ENABLE_SANITIZERS requires a C++ toolchain that can compile and link with ASan and UBSan")
endif()

add_library(orbit_sanitizers INTERFACE)
target_compile_options(orbit_sanitizers INTERFACE ${orbit_sanitizer_compile_flags})
target_link_options(orbit_sanitizers INTERFACE ${orbit_sanitizer_link_flags})

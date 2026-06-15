find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME integration_proxy_sessions
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/proxy_sessions_test.py"
        "$<TARGET_FILE:orbit>"
)

set_tests_properties(
    integration_proxy_sessions
    PROPERTIES
        TIMEOUT 45
        LABELS integration
)

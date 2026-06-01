find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME integration_two_client_forwarding
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/two_client_forwarding_test.py"
        "$<TARGET_FILE:orbit>"
)

set_tests_properties(
    integration_two_client_forwarding
    PROPERTIES
        TIMEOUT 10
        LABELS integration
)

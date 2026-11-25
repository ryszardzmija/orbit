include(GoogleTest)

function(orbit_discover_unit_tests target)
    if (ORBIT_ENABLE_SANITIZERS)
        target_link_libraries(${target} PRIVATE orbit_sanitizers)
    endif()
    gtest_discover_tests(${target}
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 15
        PROPERTIES LABELS unit TIMEOUT 15
    )
endfunction()

add_subdirectory(config)
add_subdirectory(proxy)
add_subdirectory(net)
add_subdirectory(integration)

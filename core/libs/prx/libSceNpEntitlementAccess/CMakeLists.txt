add_library(libSceNpEntitlementAccess SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceNpEntitlementAccess PRIVATE ${LIBS_INCLUDE_DIR})
configure_windows_unwind(libSceNpEntitlementAccess)

if(BUILD_TESTING)
    add_test_executable(guest_entitlement_tests ${LIBS_INCLUDE_DIR}/tests/GuestEntitlement.cpp)
    target_include_directories(guest_entitlement_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_entitlement_tests PRIVATE libSceNpEntitlementAccess libc)
    configure_windows_unwind(guest_entitlement_tests)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test-data)
    foreach(mode flags list empty invalid offline concurrent malformed missing)
        set(fixture "${CMAKE_CURRENT_BINARY_DIR}/test-data/${mode}.ini")
        add_test(NAME guest_entitlement_${mode} COMMAND guest_entitlement_tests ${mode} ${fixture})
        set_tests_properties(guest_entitlement_${mode} PROPERTIES TIMEOUT 20
            LABELS "module-functional;module:libSceNpEntitlementAccess"
            ENVIRONMENT "ANYPS5_ENTITLEMENTS=${fixture}")
        if(WIN32)
            set_tests_properties(guest_entitlement_${mode} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceNpEntitlementAccess>")
        endif()
    endforeach()
endif()

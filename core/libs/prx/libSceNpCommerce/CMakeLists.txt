add_library(libSceNpCommerce SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceNpCommerce PRIVATE ${LIBS_INCLUDE_DIR})
configure_windows_unwind(libSceNpCommerce)

if(BUILD_TESTING)
    add_test_executable(guest_commerce_tests ${LIBS_INCLUDE_DIR}/tests/GuestCommerce.cpp)
    target_include_directories(guest_commerce_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_commerce_tests PRIVATE libSceNpCommerce libc)
    configure_windows_unwind(guest_commerce_tests)
    foreach(mode legacy open2 validation lifecycle reuse concurrent icons)
        add_test(NAME guest_commerce_${mode} COMMAND guest_commerce_tests ${mode})
        set_tests_properties(guest_commerce_${mode} PROPERTIES TIMEOUT 20
            LABELS "module-functional;module:libSceNpCommerce")
        if(WIN32)
            set_tests_properties(guest_commerce_${mode} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceNpCommerce>")
        endif()
    endforeach()
endif()

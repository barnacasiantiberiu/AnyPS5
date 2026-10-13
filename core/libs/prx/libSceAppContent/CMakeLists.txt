add_library(libSceAppContent SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceAppContent PRIVATE ${LIBS_INCLUDE_DIR})
configure_windows_unwind(libSceAppContent)

if(BUILD_TESTING)
    add_test_executable(guest_appcontent_tests ${LIBS_INCLUDE_DIR}/tests/GuestAppContent.cpp)
    target_include_directories(guest_appcontent_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_appcontent_tests PRIVATE libSceAppContent libc)
    configure_windows_unwind(guest_appcontent_tests)
    add_test(NAME guest_appcontent COMMAND guest_appcontent_tests)
    set_tests_properties(guest_appcontent PROPERTIES TIMEOUT 20
        LABELS "module-functional;module:libSceAppContent" ENVIRONMENT "ANYPS5_ADDCONT_ROOT=")
    if(WIN32)
        set_tests_properties(guest_appcontent PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceAppContent>")
    endif()

    add_test_executable(guest_appcontent_addons_tests ${LIBS_INCLUDE_DIR}/tests/GuestAppContentAddons.cpp)
    target_include_directories(guest_appcontent_addons_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_appcontent_addons_tests PRIVATE libSceAppContent libc)
    configure_windows_unwind(guest_appcontent_addons_tests)
    foreach(mode queries progress mounts invalid concurrent missing bad-root removed)
        add_test(NAME guest_appcontent_${mode} COMMAND guest_appcontent_addons_tests ${mode})
        set_tests_properties(guest_appcontent_${mode} PROPERTIES TIMEOUT 20
            LABELS "module-functional;module:libSceAppContent" ENVIRONMENT "ANYPS5_ADDCONT_ROOT=installed")
        if(WIN32)
            set_tests_properties(guest_appcontent_${mode} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceAppContent>")
        endif()
    endforeach()
endif()

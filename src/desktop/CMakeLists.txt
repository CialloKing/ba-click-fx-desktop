set(BAFX_DESKTOP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    version.hpp.in
    "${BAFX_DESKTOP_GENERATED_DIR}/bafx/desktop/version.hpp"
    @ONLY
)
configure_file(
    ba-click-fx-desktop.manifest.in
    "${BAFX_DESKTOP_GENERATED_DIR}/ba-click-fx-desktop.manifest"
    @ONLY
)
configure_file(
    ba-click-fx-desktop.rc.in
    "${BAFX_DESKTOP_GENERATED_DIR}/ba-click-fx-desktop.rc"
    @ONLY
)

add_executable(
    ba_click_fx_desktop
    WIN32
        host_control.cpp
        main.cpp
        "${BAFX_DESKTOP_GENERATED_DIR}/ba-click-fx-desktop.manifest"
        "${BAFX_DESKTOP_GENERATED_DIR}/ba-click-fx-desktop.rc"
)

target_compile_features(ba_click_fx_desktop PRIVATE cxx_std_20)
target_include_directories(
    ba_click_fx_desktop
    PRIVATE
        "${BAFX_DESKTOP_GENERATED_DIR}"
)
target_link_libraries(
    ba_click_fx_desktop
    PRIVATE
        bafx::core
        bafx::config
        bafx::fx
        bafx::windows
        ole32
)
bafx_enable_warnings(ba_click_fx_desktop)

set_target_properties(
    ba_click_fx_desktop
    PROPERTIES
        FOLDER "Applications"
        OUTPUT_NAME "ba-click-fx-desktop"
)

add_custom_target(
    smoke_desktop
    COMMAND $<TARGET_FILE:ba_click_fx_desktop> --smoke-test
    DEPENDS ba_click_fx_desktop
    COMMENT "Run the interactive DirectComposition smoke test"
    VERBATIM
)
set_target_properties(smoke_desktop PROPERTIES FOLDER "Validation")

if(BAFX_POWERSHELL_EXECUTABLE)
    add_custom_target(
        verify_portable_desktop
        COMMAND
            "${BAFX_POWERSHELL_EXECUTABLE}"
            -NoProfile
            -File "${CMAKE_SOURCE_DIR}/tools/verify-portable-pe.ps1"
            -Executable $<TARGET_FILE:ba_click_fx_desktop>
            -Linker "${CMAKE_LINKER}"
        DEPENDS ba_click_fx_desktop
        COMMENT "Verify the desktop executable has no Visual C++ runtime DLL dependency"
        VERBATIM
    )
    set_target_properties(verify_portable_desktop PROPERTIES FOLDER "Validation")
endif()

option(
    BAFX_ENABLE_DESKTOP_SMOKE_TESTS
    "Register tests that require an interactive Windows desktop"
    OFF
)
if(BUILD_TESTING AND BAFX_ENABLE_DESKTOP_SMOKE_TESTS)
    add_test(
        NAME desktop_smoke
        COMMAND $<TARGET_FILE:ba_click_fx_desktop> --smoke-test
    )
    add_test(
        NAME desktop_timed_exit
        COMMAND $<TARGET_FILE:ba_click_fx_desktop> --quit-after-ms=250
    )
    set_tests_properties(
        desktop_smoke desktop_timed_exit
        PROPERTIES
            LABELS "integration;desktop"
            TIMEOUT 15
    )
endif()

if(BUILD_TESTING AND BAFX_POWERSHELL_EXECUTABLE)
    add_test(
        NAME desktop_portable_pe
        COMMAND
            "${BAFX_POWERSHELL_EXECUTABLE}"
            -NoProfile
            -File "${CMAKE_SOURCE_DIR}/tools/verify-portable-pe.ps1"
            -Executable $<TARGET_FILE:ba_click_fx_desktop>
            -Linker "${CMAKE_LINKER}"
    )
    set_tests_properties(desktop_portable_pe PROPERTIES LABELS "package")
endif()

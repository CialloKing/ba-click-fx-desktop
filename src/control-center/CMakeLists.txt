set(BAFX_CONTROL_CENTER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    BAFX.ControlCenter.rc.in
    "${BAFX_CONTROL_CENTER_GENERATED_DIR}/BAFX.ControlCenter.rc"
    @ONLY
)

# The app and UI tests compile the same implementation and feature definitions.
# Keeping one target prevents source lists from drifting as pages are split.
add_library(
    bafx_control_center_ui
    STATIC
        config_commands.cpp
        control_center_diagnostics.cpp
        control_center_controls.cpp
        control_center_display.cpp
        localization.cpp
        language_preferences.cpp
        control_center_layout.cpp
        control_center_localization.cpp
        control_center_window.cpp
        control_center_window_layout.cpp
        control_center_hotkeys.cpp
        display_state.cpp
        obs_spout_plugin_probe.cpp
        package_activation.cpp
        startup_config.cpp
        host_state.cpp
        "${PROJECT_SOURCE_DIR}/src/windows/src/package_identity.cpp"
        "${PROJECT_SOURCE_DIR}/src/windows/src/portable_paths.cpp"
        "${PROJECT_SOURCE_DIR}/src/windows/src/diagnostic_log.cpp"
        "${PROJECT_SOURCE_DIR}/src/windows/src/recording_compatibility.cpp"
        "${PROJECT_SOURCE_DIR}/src/windows/src/ipc_client.cpp"
)

add_library(bafx::control_center_ui ALIAS bafx_control_center_ui)

target_compile_features(bafx_control_center_ui PUBLIC cxx_std_20)
target_compile_definitions(
    bafx_control_center_ui
    PUBLIC
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        BAFX_RECORDING_COMPATIBILITY_APPLICATION_REVISION="${BAFX_VERSION}"
)
if(BAFX_ENABLE_SPOUT2)
    # Slim builds retain the shared config schema but must not expose an
    # output switch that their Host cannot implement.
    target_compile_definitions(bafx_control_center_ui PUBLIC BAFX_ENABLE_SPOUT2=1)
endif()
target_include_directories(
    bafx_control_center_ui
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/src/windows/include"
)
target_link_libraries(
    bafx_control_center_ui
    PUBLIC
        bafx::config
        bafx::product
        bafx::release_update
        comdlg32
        comctl32
        advapi32
        bcrypt
        ole32
        shell32
        user32
        version
)
bafx_enable_warnings(bafx_control_center_ui)

set_target_properties(bafx_control_center_ui PROPERTIES FOLDER "Libraries")

add_executable(
    bafx_control_center
    WIN32
        main.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/BAFX.ControlCenter.manifest"
        "${BAFX_CONTROL_CENTER_GENERATED_DIR}/BAFX.ControlCenter.rc"
)
target_link_libraries(bafx_control_center PRIVATE bafx::control_center_ui)
bafx_enable_warnings(bafx_control_center)

set_target_properties(
    bafx_control_center
    PROPERTIES
        FOLDER "Applications"
        OUTPUT_NAME "BAFX.ControlCenter"
)

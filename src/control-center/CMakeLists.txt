set(BAFX_CONTROL_CENTER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    BAFX.ControlCenter.rc.in
    "${BAFX_CONTROL_CENTER_GENERATED_DIR}/BAFX.ControlCenter.rc"
    @ONLY
)

add_executable(
    bafx_control_center
    WIN32
        config_commands.cpp
        control_center_window.cpp
        package_activation.cpp
        host_state.cpp
        main.cpp
        "${PROJECT_SOURCE_DIR}/src/windows/src/package_identity.cpp"
        "${PROJECT_SOURCE_DIR}/src/windows/src/portable_paths.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/BAFX.ControlCenter.manifest"
        "${BAFX_CONTROL_CENTER_GENERATED_DIR}/BAFX.ControlCenter.rc"
        "${PROJECT_SOURCE_DIR}/src/windows/src/ipc_client.cpp"
)

target_compile_features(bafx_control_center PRIVATE cxx_std_20)
target_compile_definitions(
    bafx_control_center
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
)
target_include_directories(
    bafx_control_center
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/windows/include"
)
target_link_libraries(
    bafx_control_center
    PRIVATE
        bafx::config
        comctl32
        ole32
        user32
)
bafx_enable_warnings(bafx_control_center)

set_target_properties(
    bafx_control_center
    PROPERTIES
        FOLDER "Applications"
        OUTPUT_NAME "BAFX.ControlCenter"
)

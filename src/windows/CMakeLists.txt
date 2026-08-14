add_library(
    bafx_windows
    STATIC
        src/background_capture_transition.cpp
        src/borderless_capture_access.cpp
        src/composition_renderer.cpp
        src/display_capabilities.cpp
        src/fx_gpu_renderer.cpp
        src/gpu_timestamp_profiler.cpp
        src/gpu_texture_readback.cpp
        src/ipc.cpp
        src/ipc_client.cpp
        src/overlay_window.cpp
        src/packed_fx_texture_loader.cpp
        src/packed_fx_textures.cpp
        src/package_identity.cpp
        src/portable_paths.cpp
        src/runtime_diagnostics.cpp
        src/wgc_frame_notification.cpp
        src/wgc_background_sensor.cpp
)
add_library(bafx::windows ALIAS bafx_windows)

target_include_directories(
    bafx_windows
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(bafx_windows PUBLIC cxx_std_20)
target_compile_definitions(
    bafx_windows
    PUBLIC
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
)
target_link_libraries(
    bafx_windows
    PUBLIC
        bafx::core
        bafx::fx
        d3d11
        dcomp
        d3dcompiler
        dwmapi
        dxgi
        shell32
        user32
    PRIVATE
        runtimeobject
)
bafx_enable_warnings(bafx_windows)

set_target_properties(
    bafx_windows
    PROPERTIES
        FOLDER "Libraries"
)

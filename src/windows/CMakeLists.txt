add_library(
    bafx_windows
    STATIC
        src/background_snapshot_diagnostics.cpp
        src/background_capture_transition.cpp
        src/borderless_capture_access.cpp
        src/composition_renderer.cpp
        src/display_capabilities.cpp
        src/display_color_monitor.cpp
        src/display_topology.cpp
        src/external_host_trust.cpp
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
        src/recording_compatibility.cpp
        src/runtime_diagnostics.cpp
        src/startup_registration.cpp
        src/wgc_frame_notification.cpp
        src/wgc_idle_drain_policy.cpp
        src/wgc_background_sensor.cpp
        src/wgc_runtime_capabilities.cpp
)
add_library(bafx::windows ALIAS bafx_windows)

include(CppWinRTCompatibility)
bafx_configure_cppwinrt_coroutines(
    bafx_windows
    src/borderless_capture_access.cpp
    src/display_color_monitor.cpp
    src/external_host_trust.cpp
    src/wgc_background_sensor.cpp
    src/wgc_runtime_capabilities.cpp
)

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
        BAFX_RECORDING_COMPATIBILITY_APPLICATION_REVISION="${BAFX_VERSION}"
)

# The 19041 C++/WinRT projection does not declare Windows::UI::WindowId,
# even though the newer WGC ABI can still be probed at runtime.  Keep that
# SDK build usable by making the optional Session-local path report
# InterfaceUnavailable instead of referencing a missing projected value type.
set(bafx_wgc_window_id_projection_unavailable OFF)
if(CMAKE_SYSTEM_VERSION
    AND CMAKE_SYSTEM_VERSION VERSION_LESS "10.0.26100.0")
    set(bafx_wgc_window_id_projection_unavailable ON)
endif()
if(bafx_wgc_window_id_projection_unavailable)
    target_compile_definitions(
        bafx_windows
        PRIVATE
            BAFX_WGC_WINDOW_ID_PROJECTION_UNAVAILABLE=1
    )
endif()
target_link_libraries(
    bafx_windows
    PUBLIC
        bafx::core
        bafx::fx
        advapi32
        bcrypt
        crypt32
        d3d11
        dcomp
        d3dcompiler
        dwmapi
        dxgi
        shell32
        shcore
        user32
        wintrust
    PRIVATE
        runtimeobject
)
bafx_enable_warnings(bafx_windows)

set_target_properties(
    bafx_windows
    PROPERTIES
        FOLDER "Libraries"
)

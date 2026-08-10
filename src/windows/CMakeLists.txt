add_library(
    bafx_windows
    STATIC
        src/composition_renderer.cpp
        src/display_capabilities.cpp
        src/embedded_unity_textures.cpp
        src/fx_gpu_renderer.cpp
        src/gpu_texture_readback.cpp
        src/ipc.cpp
        src/ipc_client.cpp
        src/overlay_window.cpp
        src/runtime_diagnostics.cpp
        src/wgc_background_sensor.cpp
        src/wic_texture_loader.cpp
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
        windowscodecs
)
bafx_enable_warnings(bafx_windows)

set_target_properties(
    bafx_windows
    PROPERTIES
        FOLDER "Libraries"
)

add_executable(
    bafx_core_tests
    test_main.cpp
    background_freshness_tests.cpp
    differential_bloom_tests.cpp
    intensity_tests.cpp
    material_tests.cpp
    roi_tests.cpp
    unity_bloom_tests.cpp
    unity_ring_mesh_tests.cpp
    unity_trail_mesh_tests.cpp
)

target_link_libraries(bafx_core_tests PRIVATE bafx::core)
target_compile_features(bafx_core_tests PRIVATE cxx_std_20)
bafx_enable_warnings(bafx_core_tests)

add_test(NAME core COMMAND bafx_core_tests)

set_target_properties(
    bafx_core_tests
    PROPERTIES
        FOLDER "Tests"
)

add_executable(
    bafx_fx_tests
    test_main.cpp
    fx_simulation_tests.cpp
)
target_link_libraries(bafx_fx_tests PRIVATE bafx::fx)
target_compile_features(bafx_fx_tests PRIVATE cxx_std_20)
bafx_enable_warnings(bafx_fx_tests)
add_test(NAME fx_simulation COMMAND bafx_fx_tests)
set_target_properties(bafx_fx_tests PROPERTIES FOLDER "Tests")

add_executable(
    bafx_reference_tests
    test_main.cpp
    unity_runtime_resources_tests.cpp
)
target_link_libraries(bafx_reference_tests PRIVATE bafx::reference)
target_compile_features(bafx_reference_tests PRIVATE cxx_std_20)
bafx_enable_warnings(bafx_reference_tests)
add_test(NAME unity_runtime_resources COMMAND bafx_reference_tests)
set_target_properties(bafx_reference_tests PROPERTIES FOLDER "Tests")

if(WIN32)
    add_executable(
        bafx_windows_tests
        test_main.cpp
        pointer_event_tests.cpp
    )
    target_link_libraries(bafx_windows_tests PRIVATE bafx::windows)
    target_compile_features(bafx_windows_tests PRIVATE cxx_std_20)
    bafx_enable_warnings(bafx_windows_tests)
    add_test(NAME pointer_event_coalescing COMMAND bafx_windows_tests)
    set_target_properties(bafx_windows_tests PROPERTIES FOLDER "Tests")

    add_executable(
        bafx_runtime_diagnostics_tests
        test_main.cpp
        runtime_diagnostics_tests.cpp
    )
    target_link_libraries(bafx_runtime_diagnostics_tests PRIVATE bafx::windows)
    target_compile_features(bafx_runtime_diagnostics_tests PRIVATE cxx_std_20)
    bafx_enable_warnings(bafx_runtime_diagnostics_tests)
    add_test(NAME runtime_diagnostics COMMAND bafx_runtime_diagnostics_tests)
    set_target_properties(bafx_runtime_diagnostics_tests PROPERTIES FOLDER "Tests")

    add_executable(
        bafx_embedded_texture_tests
        test_main.cpp
        embedded_unity_textures_tests.cpp
    )
    target_include_directories(
        bafx_embedded_texture_tests
        PRIVATE
            "${CMAKE_SOURCE_DIR}/src/windows/src"
    )
    target_link_libraries(
        bafx_embedded_texture_tests
        PRIVATE
            bafx::windows
            bcrypt
    )
    target_compile_features(bafx_embedded_texture_tests PRIVATE cxx_std_20)
    bafx_enable_warnings(bafx_embedded_texture_tests)
    add_test(NAME embedded_unity_textures COMMAND bafx_embedded_texture_tests)
    set_target_properties(bafx_embedded_texture_tests PROPERTIES FOLDER "Tests")

    add_executable(
        bafx_shader_contract_tests
        test_main.cpp
        fx_shader_contract_tests.cpp
    )
    target_include_directories(
        bafx_shader_contract_tests
        PRIVATE
            "${CMAKE_SOURCE_DIR}/src/windows/src"
    )
    target_link_libraries(bafx_shader_contract_tests PRIVATE d3dcompiler)
    target_compile_features(bafx_shader_contract_tests PRIVATE cxx_std_20)
    bafx_enable_warnings(bafx_shader_contract_tests)
    add_test(NAME fx_shader_contract COMMAND bafx_shader_contract_tests)
    set_target_properties(bafx_shader_contract_tests PROPERTIES FOLDER "Tests")

    # Shader errors must fail a normal build before the desktop app first runs.
    add_custom_target(
        verify_embedded_fx_shaders ALL
        COMMAND $<TARGET_FILE:bafx_shader_contract_tests>
        DEPENDS bafx_shader_contract_tests
        COMMENT "Compile and reflect all embedded FX shaders"
        VERBATIM
    )
    set_target_properties(
        verify_embedded_fx_shaders
        PROPERTIES
            FOLDER "Validation"
    )

    add_executable(
        bafx_gpu_pipeline_tests
        test_main.cpp
        fx_gpu_pipeline_tests.cpp
    )
    target_link_libraries(
        bafx_gpu_pipeline_tests
        PRIVATE
            bafx::windows
            ole32
    )
    target_compile_features(bafx_gpu_pipeline_tests PRIVATE cxx_std_20)
    bafx_enable_warnings(bafx_gpu_pipeline_tests)
    add_test(NAME gpu_pipeline_warp COMMAND bafx_gpu_pipeline_tests)
    set_tests_properties(
        gpu_pipeline_warp
        PROPERTIES
            LABELS "gpu;warp"
            TIMEOUT 30
    )
    set_target_properties(bafx_gpu_pipeline_tests PROPERTIES FOLDER "Tests")
endif()

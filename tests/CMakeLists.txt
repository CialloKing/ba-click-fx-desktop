add_executable(
    bafx_core_tests
    test_main.cpp
    background_freshness_tests.cpp
    color_space_tests.cpp
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
    bafx_config_tests
    test_main.cpp
    config_tests.cpp
)
target_link_libraries(bafx_config_tests PRIVATE bafx::config)
target_compile_features(bafx_config_tests PRIVATE cxx_std_20)
bafx_enable_warnings(bafx_config_tests)
add_test(NAME config COMMAND bafx_config_tests)
set_target_properties(bafx_config_tests PROPERTIES FOLDER "Tests")

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
    unity_particle_fixture_snapshot_tests.cpp
    unity_runtime_resources_tests.cpp
)
target_link_libraries(bafx_reference_tests PRIVATE bafx::reference)
target_compile_features(bafx_reference_tests PRIVATE cxx_std_20)
bafx_enable_warnings(bafx_reference_tests)
add_test(NAME unity_runtime_resources COMMAND bafx_reference_tests)
set_target_properties(bafx_reference_tests PROPERTIES FOLDER "Tests")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME golden_metrics_contract
        COMMAND
            "${Python3_EXECUTABLE}"
            -B
            "${CMAKE_CURRENT_SOURCE_DIR}/verify_golden_metrics_tests.py"
    )
    set_tests_properties(
        golden_metrics_contract
        PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME unity_particle_fixture_contract
        COMMAND
            "${Python3_EXECUTABLE}"
            -B
            "${CMAKE_CURRENT_SOURCE_DIR}/verify_unity_particle_fixture_tests.py"
    )
    set_tests_properties(
        unity_particle_fixture_contract
        PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            TIMEOUT 10
    )
    add_test(
        NAME unity_particle_fixture_generator_contract
        COMMAND
            "${Python3_EXECUTABLE}"
            -B
            "${CMAKE_CURRENT_SOURCE_DIR}/generate_unity_particle_fixture_tests.py"
    )
    set_tests_properties(
        unity_particle_fixture_generator_contract
        PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            TIMEOUT 10
    )
endif()

if(WIN32)
    add_executable(
        bafx_desktop_input_tests
        test_main.cpp
        pointer_frame_dispatch_tests.cpp
    )
    target_link_libraries(
        bafx_desktop_input_tests
        PRIVATE
            bafx::desktop_input
    )
    target_compile_features(bafx_desktop_input_tests PRIVATE cxx_std_20)
    bafx_enable_warnings(bafx_desktop_input_tests)
    add_test(NAME desktop_input_dispatch COMMAND bafx_desktop_input_tests)
    set_target_properties(
        bafx_desktop_input_tests
        PROPERTIES
            FOLDER "Tests"
    )

    add_executable(
        bafx_windows_tests
        test_main.cpp
        ipc_client_tests.cpp
        ipc_protocol_tests.cpp
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
        bafx_control_center_tests
        test_main.cpp
        control_center_commands_tests.cpp
        package_activation_tests.cpp
        "${PROJECT_SOURCE_DIR}/src/control-center/config_commands.cpp"
        "${PROJECT_SOURCE_DIR}/src/control-center/package_activation.cpp"
    )
    target_include_directories(
        bafx_control_center_tests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src/control-center"
    )
    target_link_libraries(
        bafx_control_center_tests
        PRIVATE
            bafx::config
            ole32
    )
    target_compile_features(bafx_control_center_tests PRIVATE cxx_std_20)
    bafx_enable_warnings(bafx_control_center_tests)
    add_test(NAME control_center_activation COMMAND bafx_control_center_tests)
    set_target_properties(bafx_control_center_tests PROPERTIES FOLDER "Tests")

    add_executable(
        bafx_identity_signer_tests
        test_main.cpp
        identity_signer_options_tests.cpp
        "${PROJECT_SOURCE_DIR}/src/identity-signer/identity_signer_options.cpp"
    )
    target_include_directories(
        bafx_identity_signer_tests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src/identity-signer"
    )
    target_compile_features(bafx_identity_signer_tests PRIVATE cxx_std_20)
    bafx_enable_warnings(bafx_identity_signer_tests)
    add_test(NAME identity_signer_arguments COMMAND bafx_identity_signer_tests)
    set_target_properties(bafx_identity_signer_tests PROPERTIES FOLDER "Tests")

    add_executable(
        bafx_packed_texture_tests
        test_main.cpp
        packed_fx_textures_tests.cpp
    )
    target_include_directories(
        bafx_packed_texture_tests
        PRIVATE
            "${CMAKE_SOURCE_DIR}/src/windows/src"
    )
    target_link_libraries(
        bafx_packed_texture_tests
        PRIVATE
            bafx::windows
            bcrypt
    )
    target_compile_features(bafx_packed_texture_tests PRIVATE cxx_std_20)
    bafx_enable_warnings(bafx_packed_texture_tests)
    add_test(NAME packed_fx_textures COMMAND bafx_packed_texture_tests)
    set_target_properties(bafx_packed_texture_tests PROPERTIES FOLDER "Tests")

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
        gpu_texture_readback_tests.cpp
    )
    target_link_libraries(
        bafx_gpu_pipeline_tests
        PRIVATE
            bafx::windows
            ole32
    )
    target_include_directories(
        bafx_gpu_pipeline_tests
        PRIVATE
            "${CMAKE_SOURCE_DIR}/src/windows/src"
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

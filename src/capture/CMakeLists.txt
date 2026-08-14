add_executable(
    ba_fx_gpu_capture
    capture_artifact_writer.cpp
    main.cpp
)

target_compile_features(ba_fx_gpu_capture PRIVATE cxx_std_20)
target_compile_definitions(
    ba_fx_gpu_capture
    PRIVATE
        BAFX_CAPTURE_VERSION="${BAFX_VERSION}"
)
target_link_libraries(
    ba_fx_gpu_capture
    PRIVATE
        bafx::core
        bafx::reference
        bafx::windows
        ole32
        windowscodecs
)
bafx_enable_warnings(ba_fx_gpu_capture)

set_target_properties(
    ba_fx_gpu_capture
    PROPERTIES
        FOLDER "Validation"
        OUTPUT_NAME "ba-click-fx-gpu-capture"
)

add_executable(
    ba_fx_composition_spike
    composition_spike_main.cpp
)
target_compile_features(ba_fx_composition_spike PRIVATE cxx_std_20)
target_compile_definitions(
    ba_fx_composition_spike
    PRIVATE
        BAFX_CAPTURE_VERSION="${BAFX_VERSION}"
)
target_link_libraries(
    ba_fx_composition_spike
    PRIVATE
        bafx::windows
        gdi32
        ole32
)
bafx_enable_warnings(ba_fx_composition_spike)
set_target_properties(
    ba_fx_composition_spike
    PROPERTIES
        FOLDER "Validation"
        OUTPUT_NAME "ba-click-fx-composition-spike"
)

option(
    BAFX_ENABLE_COMPOSITION_SPIKE_TESTS
    "Register the interactive DirectComposition/WGC composition spike"
    OFF
)
if(BUILD_TESTING AND BAFX_ENABLE_COMPOSITION_SPIKE_TESTS)
    add_test(
        NAME composition_spike_capture
        COMMAND
            ba_fx_composition_spike
            "--output=${CMAKE_CURRENT_BINARY_DIR}/ctest-composition-spike"
            --revision=ctest
            --timeout-ms=15000
    )
    set_tests_properties(
        composition_spike_capture
        PROPERTIES
            LABELS "integration;desktop;gpu;spike"
            RUN_SERIAL TRUE
            TIMEOUT 30
    )
endif()

if(BUILD_TESTING)
    add_test(
        NAME gpu_capture_cli
        COMMAND
            ba_fx_gpu_capture
            "--output=${CMAKE_CURRENT_BINARY_DIR}/ctest-output"
            --age-ms=130
            --revision=ctest
    )
    set_tests_properties(
        gpu_capture_cli
        PROPERTIES
            LABELS "gpu;warp"
            TIMEOUT 30
    )

    add_test(
        NAME gpu_capture_drag_trail_cli
        COMMAND
            ba_fx_gpu_capture
            "--output=${CMAKE_CURRENT_BINARY_DIR}/ctest-drag-trail-output"
            --case=drag-trail
            --revision=ctest
    )
    set_tests_properties(
        gpu_capture_drag_trail_cli
        PROPERTIES
            LABELS "gpu;warp"
            TIMEOUT 30
    )

    add_test(
        NAME gpu_capture_unity_particle_fixture_cli
        COMMAND
            ba_fx_gpu_capture
            "--output=${CMAKE_CURRENT_BINARY_DIR}/ctest-unity-particle-fixture-output"
            --case=unity-particle-fixture
            --all-layers
            --revision=ctest
    )
    set_tests_properties(
        gpu_capture_unity_particle_fixture_cli
        PROPERTIES
            LABELS "gpu;warp"
            TIMEOUT 30
    )
endif()

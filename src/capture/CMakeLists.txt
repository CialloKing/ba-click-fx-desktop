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
    spike_runtime.cpp
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

add_executable(
    ba_fx_wgc_lifecycle_spike
    spike_runtime.cpp
    wgc_lifecycle_spike_main.cpp
)
target_compile_features(ba_fx_wgc_lifecycle_spike PRIVATE cxx_std_20)
target_compile_definitions(
    ba_fx_wgc_lifecycle_spike
    PRIVATE
        BAFX_CAPTURE_VERSION="${BAFX_VERSION}"
)
target_link_libraries(
    ba_fx_wgc_lifecycle_spike
    PRIVATE
        bafx::windows
        gdi32
        ole32
)
bafx_enable_warnings(ba_fx_wgc_lifecycle_spike)
set_target_properties(
    ba_fx_wgc_lifecycle_spike
    PROPERTIES
        FOLDER "Validation"
        OUTPUT_NAME "ba-click-fx-wgc-lifecycle-spike"
)

add_executable(
    ba_fx_wgc_cursor_spike
    capture_artifact_writer.cpp
    spike_runtime.cpp
    wgc_cursor_spike_main.cpp
)
target_compile_features(ba_fx_wgc_cursor_spike PRIVATE cxx_std_20)
target_compile_definitions(
    ba_fx_wgc_cursor_spike
    PRIVATE
        BAFX_CAPTURE_VERSION="${BAFX_VERSION}"
)
target_link_libraries(
    ba_fx_wgc_cursor_spike
    PRIVATE
        bafx::windows
        gdi32
        ole32
        windowscodecs
)
bafx_enable_warnings(ba_fx_wgc_cursor_spike)
set_target_properties(
    ba_fx_wgc_cursor_spike
    PROPERTIES
        FOLDER "Validation"
        OUTPUT_NAME "ba-click-fx-wgc-cursor-spike"
)

add_executable(
    ba_fx_wgc_self_exclusion_spike
    capture_artifact_writer.cpp
    spike_runtime.cpp
    wgc_self_exclusion_spike_main.cpp
)
target_compile_features(ba_fx_wgc_self_exclusion_spike PRIVATE cxx_std_20)
target_compile_definitions(
    ba_fx_wgc_self_exclusion_spike
    PRIVATE
        BAFX_CAPTURE_VERSION="${BAFX_VERSION}"
)
target_link_libraries(
    ba_fx_wgc_self_exclusion_spike
    PRIVATE
        bafx::windows
        gdi32
        ole32
        windowscodecs
)
bafx_enable_warnings(ba_fx_wgc_self_exclusion_spike)
set_target_properties(
    ba_fx_wgc_self_exclusion_spike
    PROPERTIES
        FOLDER "Validation"
        OUTPUT_NAME "ba-click-fx-wgc-self-exclusion-spike"
)

add_executable(
    ba_fx_wgc_session_exclusion_spike
    capture_artifact_writer.cpp
    spike_runtime.cpp
    wgc_session_exclusion_spike_main.cpp
)
target_compile_features(ba_fx_wgc_session_exclusion_spike PRIVATE cxx_std_20)
set(bafx_capture_windows_sdk_version "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
if(NOT bafx_capture_windows_sdk_version)
    set(bafx_capture_windows_sdk_version "${CMAKE_SYSTEM_VERSION}")
endif()
target_compile_definitions(
    ba_fx_wgc_session_exclusion_spike
    PRIVATE
        BAFX_CAPTURE_VERSION="${BAFX_VERSION}"
        BAFX_CAPTURE_WINDOWS_SDK_VERSION="${bafx_capture_windows_sdk_version}"
)
target_link_libraries(
    ba_fx_wgc_session_exclusion_spike
    PRIVATE
        bafx::windows
        gdi32
        ole32
        windowscodecs
)
bafx_enable_warnings(ba_fx_wgc_session_exclusion_spike)
set_target_properties(
    ba_fx_wgc_session_exclusion_spike
    PROPERTIES
        FOLDER "Validation"
        OUTPUT_NAME "ba-click-fx-wgc-session-exclusion-spike"
)

option(
    BAFX_ENABLE_COMPOSITION_SPIKE_TESTS
    "Register the interactive DirectComposition/WGC composition spike"
    OFF
)
option(
    BAFX_ENABLE_WGC_LIFECYCLE_SPIKE_TESTS
    "Register the interactive WGC lifecycle spike"
    OFF
)
option(
    BAFX_ENABLE_WGC_CURSOR_SPIKE_TESTS
    "Register the cursor-moving WGC pixel spike"
    OFF
)
option(
    BAFX_ENABLE_WGC_SELF_EXCLUSION_SPIKE_TESTS
    "Register the WDA self-exclusion WGC pixel spike"
    OFF
)
option(
    BAFX_ENABLE_WGC_SESSION_EXCLUSION_SPIKE_TESTS
    "Register the session-local WGC exclusion pixel spike"
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

if(BUILD_TESTING AND BAFX_ENABLE_WGC_LIFECYCLE_SPIKE_TESTS)
    add_test(
        NAME wgc_lifecycle_spike_capture
        COMMAND
            ba_fx_wgc_lifecycle_spike
            "--output=${CMAKE_CURRENT_BINARY_DIR}/ctest-wgc-lifecycle-spike"
            --revision=ctest
            --timeout-ms=12000
    )
    set_tests_properties(
        wgc_lifecycle_spike_capture
        PROPERTIES
            LABELS "integration;desktop;gpu;spike"
            RUN_SERIAL TRUE
            TIMEOUT 30
    )
endif()

if(BUILD_TESTING AND BAFX_ENABLE_WGC_CURSOR_SPIKE_TESTS)
    add_test(
        NAME wgc_cursor_spike_capture
        COMMAND
            ba_fx_wgc_cursor_spike
            "--output=${CMAKE_CURRENT_BINARY_DIR}/ctest-wgc-cursor-spike"
            --revision=ctest
            --timeout-ms=12000
    )
    set_tests_properties(
        wgc_cursor_spike_capture
        PROPERTIES
            LABELS "integration;desktop;gpu;spike;cursor"
            RUN_SERIAL TRUE
            TIMEOUT 30
    )
endif()

if(BUILD_TESTING AND BAFX_ENABLE_WGC_SELF_EXCLUSION_SPIKE_TESTS)
    add_test(
        NAME wgc_self_exclusion_spike_capture
        COMMAND
            ba_fx_wgc_self_exclusion_spike
            "--output=${CMAKE_CURRENT_BINARY_DIR}/ctest-wgc-self-exclusion-spike"
            --revision=ctest
            --timeout-ms=15000
    )
    set_tests_properties(
        wgc_self_exclusion_spike_capture
        PROPERTIES
            LABELS "integration;desktop;gpu;spike;capture-exclusion"
            RUN_SERIAL TRUE
            TIMEOUT 30
    )
endif()

if(BUILD_TESTING AND BAFX_ENABLE_WGC_SESSION_EXCLUSION_SPIKE_TESTS)
    add_test(
        NAME wgc_session_exclusion_spike_capture
        COMMAND
            ba_fx_wgc_session_exclusion_spike
            "--output=${CMAKE_CURRENT_BINARY_DIR}/ctest-wgc-session-exclusion-spike"
            --revision=ctest
            --timeout-ms=15000
    )
    set_tests_properties(
        wgc_session_exclusion_spike_capture
        PROPERTIES
            LABELS "integration;desktop;gpu;spike;capture-exclusion"
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

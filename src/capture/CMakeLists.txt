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
endif()

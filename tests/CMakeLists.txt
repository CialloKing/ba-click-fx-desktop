add_executable(
    bafx_core_tests
    test_main.cpp
    background_freshness_tests.cpp
    differential_bloom_tests.cpp
    intensity_tests.cpp
    material_tests.cpp
    roi_tests.cpp
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


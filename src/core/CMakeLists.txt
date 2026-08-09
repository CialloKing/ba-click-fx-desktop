add_library(
    bafx_core
    STATIC
        src/background_freshness.cpp
        src/differential_bloom.cpp
        src/intensity.cpp
        src/material.cpp
        src/roi.cpp
        src/unity_bloom.cpp
        src/unity_ring_mesh.cpp
        src/unity_trail_mesh.cpp
)
add_library(bafx::core ALIAS bafx_core)

target_include_directories(
    bafx_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(bafx_core PUBLIC cxx_std_20)
bafx_enable_warnings(bafx_core)

set_target_properties(
    bafx_core
    PROPERTIES
        FOLDER "Libraries"
)

add_library(
    bafx_fx
    STATIC
        src/frame_bounds.cpp
        src/simulation.cpp
        src/simulation_runtime.cpp
        src/simulation_timeline.cpp
)
add_library(bafx::fx ALIAS bafx_fx)

target_include_directories(
    bafx_fx
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(bafx_fx PUBLIC cxx_std_20)
target_link_libraries(bafx_fx PUBLIC bafx::core)
bafx_enable_warnings(bafx_fx)

set_target_properties(
    bafx_fx
    PROPERTIES
        FOLDER "Libraries"
)

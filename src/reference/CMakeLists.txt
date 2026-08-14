add_library(
    bafx_reference
    STATIC
        src/unity_particle_fixture.cpp
        src/unity_runtime_resources.cpp
)
add_library(bafx::reference ALIAS bafx_reference)

target_include_directories(
    bafx_reference
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(bafx_reference PUBLIC cxx_std_20)
target_link_libraries(
    bafx_reference
    PUBLIC
        bafx::fx
    PRIVATE
        bafx::core
)
bafx_enable_warnings(bafx_reference)

set_target_properties(
    bafx_reference
    PROPERTIES
        FOLDER "Libraries"
)

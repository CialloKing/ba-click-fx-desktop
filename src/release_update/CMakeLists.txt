add_library(
    bafx_release_update
    STATIC
        update_check.cpp
        winhttp_update_transport.cpp
)
add_library(bafx::release_update ALIAS bafx_release_update)

target_include_directories(
    bafx_release_update
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(bafx_release_update PUBLIC cxx_std_20)
target_compile_definitions(
    bafx_release_update
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)
target_link_libraries(bafx_release_update PRIVATE winhttp)
bafx_enable_warnings(bafx_release_update)

set_target_properties(
    bafx_release_update
    PROPERTIES
        FOLDER "Libraries"
)

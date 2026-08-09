add_library(
    bafx_config
    STATIC
        src/config.cpp
)
add_library(bafx::config ALIAS bafx_config)

target_include_directories(
    bafx_config
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(bafx_config PUBLIC cxx_std_20)
bafx_enable_warnings(bafx_config)

set_target_properties(
    bafx_config
    PROPERTIES
        FOLDER "Libraries"
)

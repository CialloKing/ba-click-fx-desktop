cmake_minimum_required(VERSION 3.25)

project(
    ba_click_fx_desktop
    VERSION 0.1.0
    DESCRIPTION "Native Windows desktop click effects"
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

add_subdirectory(src/core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()


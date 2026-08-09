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
include(CTest)

add_subdirectory(src/core)
add_subdirectory(src/fx)
add_subdirectory(src/reference)

if(WIN32)
    add_subdirectory(src/windows)
    add_subdirectory(src/desktop)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(WIN32)
    find_program(BAFX_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if(BAFX_POWERSHELL_EXECUTABLE)
        add_custom_target(
            verify_unity_reference
            COMMAND
                "${BAFX_POWERSHELL_EXECUTABLE}"
                -NoProfile
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify-unity-reference.ps1"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Verify the external Unity/game reference without copying assets"
            VERBATIM
        )
        set_target_properties(
            verify_unity_reference
            PROPERTIES
                FOLDER "Validation"
        )
    endif()
endif()

cmake_minimum_required(VERSION 3.25)

include(cmake/Version.cmake)

project(
    ba_click_fx_desktop
    VERSION "${BAFX_VERSION_CORE}"
    DESCRIPTION "Native Windows desktop click effects"
    LANGUAGES CXX
)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(MSVC)
    # Alpha packages must run without a separately installed Visual C++ runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(CTest)

if(WIN32)
    find_program(BAFX_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
endif()

add_subdirectory(src/core)
add_subdirectory(src/fx)
add_subdirectory(src/reference)

if(WIN32)
    add_subdirectory(src/windows)
    add_subdirectory(src/desktop)
    add_subdirectory(src/capture)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(WIN32)
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

include(cmake/Packaging.cmake)

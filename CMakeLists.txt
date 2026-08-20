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

option(
    BAFX_ENABLE_SPOUT2
    "Enable the optional Spout2 DirectX sender"
    OFF
)

set(BAFX_SPOUT2_STATIC_CRT OFF)
if(DEFINED VCPKG_TARGET_TRIPLET
    AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(BAFX_SPOUT2_STATIC_CRT ON)
elseif(DEFINED VCPKG_CRT_LINKAGE
    AND VCPKG_CRT_LINKAGE STREQUAL "static")
    set(BAFX_SPOUT2_STATIC_CRT ON)
endif()

if(MSVC)
    # Release packages must run without a separately installed Visual C++ runtime.
    # The vcpkg x64-windows Spout2 binary is built with the dynamic CRT. Keep
    # the whole static-library graph on the same CRT when that optional output
    # is enabled; mixing /MT and /MD is rejected by the linker and can also
    # split ownership of standard-library allocations across module boundaries.
    if(BAFX_ENABLE_SPOUT2 AND NOT BAFX_SPOUT2_STATIC_CRT)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(CTest)

if(BAFX_ENABLE_SPOUT2)
    if((DEFINED VCPKG_TARGET_TRIPLET
            AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
        OR (DEFINED VCPKG_CRT_LINKAGE
            AND VCPKG_CRT_LINKAGE STREQUAL "static"))
        # The static vcpkg port removes shared Spout binaries after install,
        # while its generated export file still checks those removed files.
        # Import only the DX static library we actually consume and keep the
        # project independent from that broken shared-target check.
        find_path(
            BAFX_SPOUT2_INCLUDE_DIR
            NAMES SpoutDX/SpoutDX.h
            REQUIRED)
        set(BAFX_SPOUT2_TRIPLET_ROOT
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        set(BAFX_SPOUT2_RELEASE_LIBRARY
            "${BAFX_SPOUT2_TRIPLET_ROOT}/lib/SpoutDX_static.lib")
        set(BAFX_SPOUT2_DEBUG_LIBRARY
            "${BAFX_SPOUT2_TRIPLET_ROOT}/debug/lib/SpoutDX_static.lib")
        if(NOT EXISTS "${BAFX_SPOUT2_RELEASE_LIBRARY}"
            OR NOT EXISTS "${BAFX_SPOUT2_DEBUG_LIBRARY}")
            message(FATAL_ERROR
                "Static Spout2 triplet is missing debug/release SpoutDX_static.lib")
        endif()
        add_library(Spout2::SpoutDX_static STATIC IMPORTED GLOBAL)
        set_target_properties(
            Spout2::SpoutDX_static
            PROPERTIES
                IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
                IMPORTED_LOCATION_DEBUG "${BAFX_SPOUT2_DEBUG_LIBRARY}"
                IMPORTED_LOCATION_RELEASE "${BAFX_SPOUT2_RELEASE_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${BAFX_SPOUT2_INCLUDE_DIR}")
    else()
        find_package(Spout2 CONFIG REQUIRED)
    endif()
endif()

if(WIN32)
    if(DEFINED BAFX_POWERSHELL_EXECUTABLE
        AND NOT EXISTS "${BAFX_POWERSHELL_EXECUTABLE}")
        # Microsoft Store updates replace their versioned install directory.
        # Drop a stale cached path so an existing build tree remains usable.
        unset(BAFX_POWERSHELL_EXECUTABLE CACHE)
    endif()
    find_program(BAFX_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
endif()

add_subdirectory(src/core)
add_subdirectory(src/config)
add_subdirectory(src/fx)
add_subdirectory(src/reference)

if(WIN32)
    add_subdirectory(src/windows)
    add_subdirectory(src/desktop)
    add_subdirectory(src/control-center)
    add_subdirectory(src/capture)
    add_subdirectory(src/identity-signer)
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

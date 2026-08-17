function(bafx_configure_cppwinrt_coroutines target_name)
    if(NOT WIN32 OR NOT MSVC)
        return()
    endif()
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "Unknown C++/WinRT target: ${target_name}")
    endif()
    if(NOT ARGN)
        message(FATAL_ERROR "C++/WinRT compatibility requires source files")
    endif()

    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(cppwinrt_probe_cmake_flags)
    if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        # A nested Visual Studio try_compile otherwise selects the newest
        # installed SDK instead of the SDK selected by the product build.
        list(
            APPEND
            cppwinrt_probe_cmake_flags
            "-DCMAKE_SYSTEM_VERSION:STRING=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
        )
    endif()
    set(
        cppwinrt_probe_source
        [=[
#include <winrt/base.h>

int cppwinrtCoroutineProbe()
{
    return 0;
}
]=]
    )
    try_compile(
        cppwinrt_supports_standard_coroutines
        SOURCE_FROM_CONTENT
            cppwinrt_standard_coroutine_probe.cpp
            "${cppwinrt_probe_source}"
        CMAKE_FLAGS ${cppwinrt_probe_cmake_flags}
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        NO_CACHE
        OUTPUT_VARIABLE cppwinrt_standard_probe_output
    )
    if(cppwinrt_supports_standard_coroutines)
        return()
    endif()

    # SDK 19041's projection hard-codes the retired coroutine TS. Restrict the
    # compiler fallback to WinRT consumers so the rest of the target stays on
    # the standard C++20 coroutine ABI.
    set(
        cppwinrt_legacy_probe_source
        [=[
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <winrt/base.h>

int cppwinrtCoroutineProbe()
{
    return 0;
}
]=]
    )
    set(
        cppwinrt_legacy_cmake_flags
        ${cppwinrt_probe_cmake_flags}
        "-DCOMPILE_DEFINITIONS=/await"
    )
    try_compile(
        cppwinrt_supports_legacy_coroutines
        SOURCE_FROM_CONTENT
            cppwinrt_legacy_coroutine_probe.cpp
            "${cppwinrt_legacy_probe_source}"
        CMAKE_FLAGS ${cppwinrt_legacy_cmake_flags}
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        NO_CACHE
        OUTPUT_VARIABLE cppwinrt_legacy_probe_output
    )
    if(NOT cppwinrt_supports_legacy_coroutines)
        message(
            FATAL_ERROR
            "The selected C++/WinRT projection supports neither standard "
            "C++20 nor the MSVC coroutine TS fallback.\n"
            "Standard probe:\n${cppwinrt_standard_probe_output}\n"
            "Legacy probe:\n${cppwinrt_legacy_probe_output}"
        )
    endif()

    set_source_files_properties(
        ${ARGN}
        TARGET_DIRECTORY ${target_name}
        PROPERTIES
            COMPILE_DEFINITIONS
                _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
            COMPILE_OPTIONS /await
    )
    message(
        STATUS
        "Using the legacy MSVC coroutine TS for the selected C++/WinRT projection"
    )
endfunction()

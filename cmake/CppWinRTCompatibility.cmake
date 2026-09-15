function(bafx_cppwinrt_probe_environment sdk_output flags_output)
    set(cppwinrt_probe_cmake_flags)
    set(cppwinrt_selected_windows_sdk "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    if(NOT cppwinrt_selected_windows_sdk
        AND DEFINED CACHE{CMAKE_SYSTEM_VERSION})
        get_property(
            cppwinrt_selected_windows_sdk
            CACHE CMAKE_SYSTEM_VERSION
            PROPERTY VALUE
        )
    endif()
    if(cppwinrt_selected_windows_sdk
        AND cppwinrt_selected_windows_sdk MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
        # A nested Visual Studio try_compile otherwise selects the newest
        # installed SDK instead of the SDK selected by the product build.
        list(
            APPEND
            cppwinrt_probe_cmake_flags
            "-DCMAKE_SYSTEM_VERSION:STRING=${cppwinrt_selected_windows_sdk}"
        )
        # Pin the projection include path as well: a nested VS project can
        # select a newer installed SDK despite CMAKE_SYSTEM_VERSION.
        find_path(
            cppwinrt_projection_directory
            NAMES winrt/base.h
            PATHS
                "$ENV{CMAKE_WINDOWS_KITS_10_DIR}"
                "$ENV{WindowsSdkDir}"
                "[HKEY_LOCAL_MACHINE/SOFTWARE/Microsoft/Windows Kits/Installed Roots;KitsRoot10]"
            PATH_SUFFIXES "Include/${cppwinrt_selected_windows_sdk}/cppwinrt"
            NO_DEFAULT_PATH
            NO_CACHE
            REQUIRED
        )
        list(APPEND cppwinrt_probe_cmake_flags
            "-DINCLUDE_DIRECTORIES:STRING=${cppwinrt_projection_directory}")
    endif()
    set(${sdk_output} "${cppwinrt_selected_windows_sdk}" PARENT_SCOPE)
    set(${flags_output} "${cppwinrt_probe_cmake_flags}" PARENT_SCOPE)
endfunction()

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
    bafx_cppwinrt_probe_environment(
        cppwinrt_selected_windows_sdk cppwinrt_probe_cmake_flags)
    if(cppwinrt_selected_windows_sdk)
        message(
            STATUS
            "C++/WinRT coroutine probe SDK: ${cppwinrt_selected_windows_sdk}"
        )
    endif()
    set(
        BAFX_CPPWINRT_LEGACY_COROUTINES
        OFF
        CACHE BOOL
        "Use the legacy MSVC coroutine TS for the selected C++/WinRT projection"
        FORCE
    )
    # The Windows 10 (19041) C++/WinRT projection is known to require the
    # retired MSVC coroutine TS.  A nested Visual Studio try_compile can
    # silently include a newer projection even when CMAKE_SYSTEM_VERSION is
    # pinned, so a successful standard probe is not evidence that the product
    # compile will work.  Select the conservative path for this SDK family;
    # newer SDKs still use the capability probe below.
    set(cppwinrt_requires_legacy_coroutines FALSE)
    if(cppwinrt_selected_windows_sdk
        AND cppwinrt_selected_windows_sdk VERSION_LESS "10.0.22000.0")
        set(cppwinrt_requires_legacy_coroutines TRUE)
        message(
            STATUS
            "C++/WinRT legacy coroutine path required by Windows SDK ${cppwinrt_selected_windows_sdk}"
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
    if(NOT cppwinrt_requires_legacy_coroutines)
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

    set(
        BAFX_CPPWINRT_LEGACY_COROUTINES
        ON
        CACHE BOOL
        "Use the legacy MSVC coroutine TS for the selected C++/WinRT projection"
        FORCE
    )
    set_source_files_properties(
        ${ARGN}
        TARGET_DIRECTORY ${target_name}
        PROPERTIES
            COMPILE_DEFINITIONS
                _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
            # SDK 19041's projection includes <experimental/coroutine> and
            # relies on non-dependent lookup in its generated headers.  Keep
            # the target on C++20 (the sources use std::span), while selecting
            # the old coroutine switch and lookup behavior for WinRT files.
            COMPILE_OPTIONS
                "/await;/Zc:twoPhase-"
    )
    message(
        STATUS
        "Using the legacy MSVC coroutine TS for the selected C++/WinRT projection"
    )
endfunction()

function(bafx_configure_wgc_window_id_projection target_name)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    bafx_cppwinrt_probe_environment(wgc_selected_sdk wgc_probe_cmake_flags)
    if(BAFX_CPPWINRT_LEGACY_COROUTINES)
        list(APPEND wgc_probe_cmake_flags
            "-DCOMPILE_DEFINITIONS=/await /Zc:twoPhase- /D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS")
    endif()
    # This tests only the declarations/collections needed by our local WGC
    # ABI. The build machine's OS cannot tell us what the target OS supports.
    try_compile(
        wgc_window_id_projection_available
        SOURCE_FROM_CONTENT wgc_window_id_projection_probe.cpp [=[
#define ENABLE_WINRT_EXPERIMENTAL_TYPES
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>

void wgcWindowIdProjectionProbe()
{
    using winrt::Windows::UI::WindowId;
    using namespace winrt::Windows::Foundation::Collections;
    auto values = winrt::single_threaded_vector<WindowId>();
    values.Append(WindowId{1U});
    auto iterable = values.as<IIterable<WindowId>>();
    IVectorView<WindowId> view = values.GetView();
    (void)winrt::get_abi(iterable);
    (void)view.GetAt(0U).Value;
}
]=]
        CMAKE_FLAGS ${wgc_probe_cmake_flags}
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        NO_CACHE
        OUTPUT_VARIABLE wgc_window_id_probe_output
    )
    set(BAFX_WGC_WINDOW_ID_PROJECTION_AVAILABLE
        "${wgc_window_id_projection_available}" CACHE INTERNAL
        "Whether the selected C++/WinRT projection supports WGC WindowId collections"
        FORCE)
    message(STATUS
        "WGC WindowId projection: SDK=${wgc_selected_sdk}; available=${wgc_window_id_projection_available}")
    if(NOT wgc_window_id_projection_available)
        message(STATUS "WGC WindowId projection probe failed:\n${wgc_window_id_probe_output}")
        target_compile_definitions(${target_name} PRIVATE
            BAFX_WGC_WINDOW_ID_PROJECTION_UNAVAILABLE=1)
    endif()
endfunction()

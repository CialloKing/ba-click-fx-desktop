set(BAFX_VERSION "0.1.0-alpha.16")

if(NOT BAFX_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9A-Za-z.-]+))?$")
    message(FATAL_ERROR "BAFX_VERSION must be a semantic version")
endif()

set(BAFX_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(BAFX_VERSION_MINOR "${CMAKE_MATCH_2}")
set(BAFX_VERSION_PATCH "${CMAKE_MATCH_3}")
set(BAFX_VERSION_PRERELEASE "${CMAKE_MATCH_5}")

# Windows file versions are four unsigned 16-bit components. Alpha builds use
# their prerelease number as the revision so Explorer can distinguish releases.
set(BAFX_VERSION_REVISION 0)
if(BAFX_VERSION_PRERELEASE MATCHES "^alpha\\.([0-9]+)$")
    set(BAFX_VERSION_REVISION "${CMAKE_MATCH_1}")
    if(BAFX_VERSION_REVISION GREATER 65535)
        message(FATAL_ERROR "BAFX alpha revision exceeds the Windows version range")
    endif()
endif()

set(
    BAFX_VERSION_CORE
    "${BAFX_VERSION_MAJOR}.${BAFX_VERSION_MINOR}.${BAFX_VERSION_PATCH}"
)

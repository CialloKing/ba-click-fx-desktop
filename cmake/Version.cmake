set(BAFX_VERSION "0.2.0")

if(NOT BAFX_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "BAFX_VERSION must use MAJOR.MINOR.PATCH")
endif()

set(BAFX_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(BAFX_VERSION_MINOR "${CMAKE_MATCH_2}")
set(BAFX_VERSION_PATCH "${CMAKE_MATCH_3}")
# Windows file versions are four unsigned 16-bit components. Public versions
# stay three-component, so the revision is fixed at zero.
set(BAFX_VERSION_REVISION 0)
foreach(BAFX_VERSION_COMPONENT IN ITEMS
        BAFX_VERSION_MAJOR
        BAFX_VERSION_MINOR
        BAFX_VERSION_PATCH)
    if(${${BAFX_VERSION_COMPONENT}} GREATER 65535)
        message(FATAL_ERROR
            "${BAFX_VERSION_COMPONENT} exceeds the Windows version range")
    endif()
endforeach()

set(
    BAFX_VERSION_CORE
    "${BAFX_VERSION_MAJOR}.${BAFX_VERSION_MINOR}.${BAFX_VERSION_PATCH}"
)

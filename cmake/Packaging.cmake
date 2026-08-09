if(NOT WIN32)
    return()
endif()

install(
    TARGETS ba_click_fx_desktop
    RUNTIME DESTINATION "."
)
install(
    FILES "${CMAKE_SOURCE_DIR}/LICENSE"
    DESTINATION "."
    RENAME "LICENSE.txt"
)
install(
    FILES
        "${CMAKE_SOURCE_DIR}/SUPPORT.md"
        "${CMAKE_SOURCE_DIR}/ASSET-MANIFEST.md"
    DESTINATION "."
)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "ba-click-fx-desktop")
set(CPACK_PACKAGE_VENDOR "ba-click-fx-desktop contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Windows desktop click effects")
set(CPACK_PACKAGE_VERSION "${BAFX_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "ba-click-fx-desktop-${BAFX_VERSION}-windows-x64")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_PACKAGE_CHECKSUM "SHA256")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

include(CPack)

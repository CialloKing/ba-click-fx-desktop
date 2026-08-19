if(NOT WIN32)
    return()
endif()

install(
    TARGETS ba_click_fx_desktop
    RUNTIME DESTINATION "."
)
install(
    TARGETS bafx_control_center
    RUNTIME DESTINATION "."
)
install(
    FILES "${CMAKE_SOURCE_DIR}/LICENSE"
    DESTINATION "."
    RENAME "LICENSE.txt"
)
install(
    FILES "${CMAKE_SOURCE_DIR}/SUPPORT.md"
    DESTINATION "."
)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "ba-click-fx-desktop")
set(CPACK_PACKAGE_VENDOR "ba-click-fx-desktop contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Windows desktop click effects")
set(CPACK_PACKAGE_VERSION "${BAFX_VERSION}")
if(BAFX_ENABLE_SPOUT2)
    set(BAFX_PACKAGE_VARIANT_SUFFIX "")
else()
    set(BAFX_PACKAGE_VARIANT_SUFFIX "-slim")
endif()
set(CPACK_PACKAGE_FILE_NAME "ba-click-fx-desktop-${BAFX_VERSION}${BAFX_PACKAGE_VARIANT_SUFFIX}-windows-x64")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_PACKAGE_CHECKSUM "SHA256")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

set(BAFX_ALPHA_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")
include(CPack)

if(BAFX_POWERSHELL_EXECUTABLE)
    add_custom_target(
        verify_alpha_package
        COMMAND
            "${BAFX_POWERSHELL_EXECUTABLE}"
            -NoProfile
            -File "${CMAKE_SOURCE_DIR}/tools/verify-alpha-package.ps1"
            -Package "${CPACK_PACKAGE_DIRECTORY}/${BAFX_ALPHA_PACKAGE_FILE_NAME}.zip"
            -ExpectedVersion "${BAFX_VERSION}"
            -Linker "${CMAKE_LINKER}"
            -PortableVerifier "${CMAKE_SOURCE_DIR}/tools/verify-portable-pe.ps1"
        COMMENT "Verify, extract, and smoke-test the Alpha ZIP"
        VERBATIM
    )
    set_target_properties(verify_alpha_package PROPERTIES FOLDER "Validation")

    add_custom_target(
        verify_user_installer_contract
        COMMAND
            "${BAFX_POWERSHELL_EXECUTABLE}"
            -NoProfile
            -File "${CMAKE_SOURCE_DIR}/tools/verify-user-installer-contract.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}"
        COMMENT "Verify the ordinary-user installer source contracts"
        VERBATIM
    )
    set_target_properties(
        verify_user_installer_contract
        PROPERTIES
            FOLDER "Validation"
    )

    add_custom_target(
        package_user_installer
        COMMAND
            "${BAFX_POWERSHELL_EXECUTABLE}"
            -NoProfile
            -File "${CMAKE_SOURCE_DIR}/tools/package-user-installer.ps1"
            -OutputDirectory "${CMAKE_SOURCE_DIR}/artifacts/local"
            -SkipBuild
        DEPENDS
            ba_click_fx_desktop
            bafx_control_center
            bafx_identity_signer
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Build the ordinary-user installer from Release binaries"
        VERBATIM
    )
    set_target_properties(package_user_installer PROPERTIES FOLDER "Packaging")

    if(BUILD_TESTING)
        add_test(
            NAME user_installer_contract
            COMMAND
                "${BAFX_POWERSHELL_EXECUTABLE}"
                -NoProfile
                -File "${CMAKE_SOURCE_DIR}/tools/verify-user-installer-contract.ps1"
                -RepositoryRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(
            user_installer_contract
            PROPERTIES
                LABELS "packaging;contract"
                TIMEOUT 30
        )
    endif()
endif()

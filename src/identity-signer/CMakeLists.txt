add_executable(
    bafx_identity_signer
    appx_signer.cpp
    identity_signer_options.cpp
    main.cpp
)

target_compile_features(bafx_identity_signer PRIVATE cxx_std_20)
target_compile_definitions(
    bafx_identity_signer
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
)
target_link_libraries(
    bafx_identity_signer
    PRIVATE
        advapi32
        crypt32
        ncrypt
        wintrust
)
bafx_enable_warnings(bafx_identity_signer)

set_target_properties(
    bafx_identity_signer
    PROPERTIES
        FOLDER "Applications"
        OUTPUT_NAME "BAFX.IdentitySigner"
)

#pragma once

#include "bafx/windows/package_identity.hpp"

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace bafx::windows
{

enum class ExternalHostTrustStatus : std::uint8_t
{
    Trusted,
    NotPackaged,
    IdentityIncomplete,
    StateMissing,
    StateInvalid,
    IdentityMismatch,
    UserMismatch,
    ExternalLocationMismatch,
    ImagePathMismatch,
    PathUnprotected,
    HostHashMismatch,
    StagedHostHashMismatch,
    PackageHashMismatch,
    PackageSignatureInvalid,
    SignerCertificateMismatch,
    CertificateStoreMismatch,
    CertificateInvalid,
    Failed
};

struct ExternalHostTrustResult
{
    ExternalHostTrustStatus status{ExternalHostTrustStatus::Failed};
    HRESULT error{E_UNEXPECTED};
    std::string statePath{};
    std::string expectedHostSha256{};
    std::string observedHostSha256{};
    std::string expectedPackageSha256{};
    std::string observedPackageSha256{};
    std::string expectedCertificateSha256{};
    std::string observedCertificateSha256{};
};

[[nodiscard]] ExternalHostTrustResult queryExternalHostTrust(
    const PackageIdentityInfo& identity) noexcept;

[[nodiscard]] bool externalHostTrusted(
    const ExternalHostTrustResult& result) noexcept;

[[nodiscard]] std::string_view externalHostTrustStatusName(
    ExternalHostTrustStatus status) noexcept;

[[nodiscard]] std::string externalHostTrustDiagnostic(
    const ExternalHostTrustResult& result);

}

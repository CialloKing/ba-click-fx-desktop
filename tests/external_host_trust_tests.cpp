#include "test_support.hpp"

#include "bafx/windows/external_host_trust.hpp"

#include <string>

BAFX_TEST(external_host_trust_expiring_certificate_is_actionable)
{
    bafx::windows::ExternalHostTrustResult result{};
    result.status = bafx::windows::ExternalHostTrustStatus::CertificateExpiringSoon;
    result.error = S_OK;
    result.certificateExpiringSoon = true;

    BAFX_CHECK(bafx::windows::externalHostTrusted(result));
    BAFX_CHECK(
        bafx::windows::externalHostTrustStatusName(result.status)
        == "certificate-expiring-soon");
    const std::string diagnostic =
        bafx::windows::externalHostTrustDiagnostic(result);
    BAFX_CHECK(diagnostic.find("CertificateExpiringSoon=true")
        != std::string::npos);
}

BAFX_TEST(external_host_trust_expired_certificate_fails_closed)
{
    bafx::windows::ExternalHostTrustResult result{};
    result.status = bafx::windows::ExternalHostTrustStatus::CertificateInvalid;
    result.error = CERT_E_EXPIRED;

    BAFX_CHECK(!bafx::windows::externalHostTrusted(result));
    BAFX_CHECK(
        bafx::windows::externalHostTrustStatusName(result.status)
        == "certificate-invalid");
}

BAFX_TEST(external_host_trust_state_pair_mismatch_is_named)
{
    bafx::windows::ExternalHostTrustResult result{};
    result.status = bafx::windows::ExternalHostTrustStatus::StatePairMismatch;
    result.error = E_ACCESSDENIED;

    BAFX_CHECK(!bafx::windows::externalHostTrusted(result));
    BAFX_CHECK(
        bafx::windows::externalHostTrustStatusName(result.status)
        == "state-pair-mismatch");
}

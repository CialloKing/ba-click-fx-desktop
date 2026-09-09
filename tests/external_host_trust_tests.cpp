#include "test_support.hpp"

#include "bafx/windows/external_host_trust.hpp"

#include <string>

BAFX_TEST(external_host_trust_valid_certificate_is_trusted)
{
    bafx::windows::ExternalHostTrustResult result{};
    result.status = bafx::windows::ExternalHostTrustStatus::Trusted;
    result.error = S_OK;

    BAFX_CHECK(bafx::windows::externalHostTrusted(result));
    BAFX_CHECK(
        bafx::windows::externalHostTrustStatusName(result.status)
        == "trusted");
    const std::string diagnostic =
        bafx::windows::externalHostTrustDiagnostic(result);
    BAFX_CHECK(diagnostic.find("Package.ExternalHostTrust=trusted")
        != std::string::npos);
    BAFX_CHECK(diagnostic.find("Certificate") == std::string::npos);
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

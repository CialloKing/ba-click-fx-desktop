#include "bafx/windows/external_host_trust.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/portable_paths.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <softpub.h>
#include <wincrypt.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::windows
{
namespace
{

namespace fs = std::filesystem;
using winrt::Windows::Data::Json::JsonObject;

constexpr std::size_t sha1ByteCount = 20U;
constexpr std::size_t sha256ByteCount = 32U;
constexpr std::uintmax_t maximumStateBytes = 128U * 1024U;

struct InstallState
{
    std::string packageName{};
    std::string applicationId{};
    std::string publisher{};
    std::string packageVersion{};
    std::string packageFullName{};
    std::string packageFamilyName{};
    std::string certificateThumbprint{};
    std::string certificateSha256{};
    std::string externalLocation{};
    std::string installedUserSid{};
    std::string hostFile{};
    std::string hostSha256{};
    std::string packageFile{};
    std::string packageSha256{};
};

class AlgorithmHandle final
{
public:
    AlgorithmHandle()
    {
        const NTSTATUS status = BCryptOpenAlgorithmProvider(
            &handle_,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0U);
        if (!BCRYPT_SUCCESS(status))
        {
            throw HResultError(HRESULT_FROM_NT(status), "BCryptOpenAlgorithmProvider");
        }
    }

    ~AlgorithmHandle()
    {
        if (handle_ != nullptr)
        {
            static_cast<void>(BCryptCloseAlgorithmProvider(handle_, 0U));
        }
    }

    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;

    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    BCRYPT_ALG_HANDLE handle_{nullptr};
};

class Sha256Hasher final
{
public:
    Sha256Hasher()
    {
        DWORD bytesWritten = 0U;
        NTSTATUS status = BCryptGetProperty(
            algorithm_.get(),
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength_),
            sizeof(objectLength_),
            &bytesWritten,
            0U);
        if (!BCRYPT_SUCCESS(status) || bytesWritten != sizeof(objectLength_))
        {
            throw HResultError(
                BCRYPT_SUCCESS(status) ? E_UNEXPECTED : HRESULT_FROM_NT(status),
                "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)");
        }
        DWORD digestLength = 0U;
        status = BCryptGetProperty(
            algorithm_.get(),
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestLength),
            sizeof(digestLength),
            &bytesWritten,
            0U);
        if (!BCRYPT_SUCCESS(status)
            || bytesWritten != sizeof(digestLength)
            || digestLength != sha256ByteCount)
        {
            throw HResultError(
                BCRYPT_SUCCESS(status) ? E_UNEXPECTED : HRESULT_FROM_NT(status),
                "BCryptGetProperty(BCRYPT_HASH_LENGTH)");
        }

        hashObject_.resize(objectLength_);
        status = BCryptCreateHash(
            algorithm_.get(),
            &handle_,
            hashObject_.data(),
            objectLength_,
            nullptr,
            0U,
            0U);
        if (!BCRYPT_SUCCESS(status))
        {
            throw HResultError(HRESULT_FROM_NT(status), "BCryptCreateHash");
        }
    }

    ~Sha256Hasher()
    {
        if (handle_ != nullptr)
        {
            static_cast<void>(BCryptDestroyHash(handle_));
        }
    }

    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    void append(const std::span<const std::byte> bytes)
    {
        if (bytes.size() > (std::numeric_limits<ULONG>::max)())
        {
            throw HResultError(E_INVALIDARG, "BCryptHashData length");
        }
        const NTSTATUS status = BCryptHashData(
            handle_,
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
            static_cast<ULONG>(bytes.size()),
            0U);
        if (!BCRYPT_SUCCESS(status))
        {
            throw HResultError(HRESULT_FROM_NT(status), "BCryptHashData");
        }
    }

    [[nodiscard]] std::array<std::byte, sha256ByteCount> finish()
    {
        std::array<std::byte, sha256ByteCount> digest{};
        const NTSTATUS status = BCryptFinishHash(
            handle_,
            reinterpret_cast<PUCHAR>(digest.data()),
            static_cast<ULONG>(digest.size()),
            0U);
        if (!BCRYPT_SUCCESS(status))
        {
            throw HResultError(HRESULT_FROM_NT(status), "BCryptFinishHash");
        }
        return digest;
    }

private:
    AlgorithmHandle algorithm_{};
    BCRYPT_HASH_HANDLE handle_{nullptr};
    DWORD objectLength_{0U};
    std::vector<UCHAR> hashObject_{};
};

class CertificateStore final
{
public:
    explicit CertificateStore(const wchar_t* name)
        : handle_(CertOpenStore(
            CERT_STORE_PROV_SYSTEM_W,
            0U,
            0U,
            CERT_SYSTEM_STORE_LOCAL_MACHINE
                | CERT_STORE_OPEN_EXISTING_FLAG
                | CERT_STORE_READONLY_FLAG,
            name))
    {
        if (handle_ == nullptr)
        {
            throwLastError("CertOpenStore");
        }
    }

    ~CertificateStore()
    {
        if (handle_ != nullptr)
        {
            static_cast<void>(CertCloseStore(handle_, 0U));
        }
    }

    CertificateStore(const CertificateStore&) = delete;
    CertificateStore& operator=(const CertificateStore&) = delete;

    [[nodiscard]] HCERTSTORE get() const noexcept
    {
        return handle_;
    }

private:
    HCERTSTORE handle_{nullptr};
};

class CertificateContext final
{
public:
    explicit CertificateContext(PCCERT_CONTEXT context = nullptr) noexcept
        : context_(context)
    {
    }

    ~CertificateContext()
    {
        if (context_ != nullptr)
        {
            CertFreeCertificateContext(context_);
        }
    }

    CertificateContext(const CertificateContext&) = delete;
    CertificateContext& operator=(const CertificateContext&) = delete;

    [[nodiscard]] PCCERT_CONTEXT get() const noexcept
    {
        return context_;
    }

private:
    PCCERT_CONTEXT context_{nullptr};
};

class WinTrustState final
{
public:
    explicit WinTrustState(const fs::path& path)
    {
        fileInfo_.cbStruct = sizeof(fileInfo_);
        fileInfo_.pcwszFilePath = path.c_str();
        trustData_.cbStruct = sizeof(trustData_);
        trustData_.dwUIChoice = WTD_UI_NONE;
        trustData_.fdwRevocationChecks = WTD_REVOKE_NONE;
        trustData_.dwUnionChoice = WTD_CHOICE_FILE;
        trustData_.pFile = &fileInfo_;
        trustData_.dwStateAction = WTD_STATEACTION_VERIFY;
        trustData_.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        result_ = WinVerifyTrust(nullptr, &policy_, &trustData_);
    }

    ~WinTrustState()
    {
        trustData_.dwStateAction = WTD_STATEACTION_CLOSE;
        static_cast<void>(WinVerifyTrust(nullptr, &policy_, &trustData_));
    }

    WinTrustState(const WinTrustState&) = delete;
    WinTrustState& operator=(const WinTrustState&) = delete;

    [[nodiscard]] LONG result() const noexcept
    {
        return result_;
    }

    [[nodiscard]] PCCERT_CONTEXT signerCertificate() const noexcept
    {
        CRYPT_PROVIDER_DATA* const provider =
            WTHelperProvDataFromStateData(trustData_.hWVTStateData);
        if (provider == nullptr)
        {
            return nullptr;
        }
        CRYPT_PROVIDER_SGNR* const signer = WTHelperGetProvSignerFromChain(
            provider,
            0U,
            FALSE,
            0U);
        if (signer == nullptr
            || signer->csCertChain == 0U
            || signer->pasCertChain == nullptr)
        {
            return nullptr;
        }
        return signer->pasCertChain[0].pCert;
    }

private:
    WINTRUST_FILE_INFO fileInfo_{};
    WINTRUST_DATA trustData_{};
    GUID policy_ = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG result_{TRUST_E_SUBJECT_NOT_TRUSTED};
};

[[nodiscard]] std::string hexHresult(const HRESULT error)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << static_cast<unsigned long>(error);
    return stream.str();
}

[[nodiscard]] std::string hexBytes(const std::span<const std::byte> bytes)
{
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (const std::byte byte : bytes)
    {
        stream << std::setw(2) << std::to_integer<unsigned int>(byte);
    }
    return stream.str();
}

[[nodiscard]] std::optional<std::vector<std::byte>> parseHex(
    const std::string_view value)
{
    if ((value.size() % 2U) != 0U)
    {
        return std::nullopt;
    }
    const auto nibble = [](const char character) noexcept -> int
    {
        if (character >= '0' && character <= '9')
        {
            return character - '0';
        }
        if (character >= 'A' && character <= 'F')
        {
            return character - 'A' + 10;
        }
        if (character >= 'a' && character <= 'f')
        {
            return character - 'a' + 10;
        }
        return -1;
    };

    std::vector<std::byte> bytes(value.size() / 2U);
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        const int high = nibble(value[index * 2U]);
        const int low = nibble(value[index * 2U + 1U]);
        if (high < 0 || low < 0)
        {
            return std::nullopt;
        }
        bytes[index] = static_cast<std::byte>((high << 4) | low);
    }
    return bytes;
}

[[nodiscard]] std::string canonicalHex(const std::string_view value)
{
    const std::optional<std::vector<std::byte>> parsed = parseHex(value);
    if (!parsed.has_value())
    {
        throw HResultError(E_INVALIDARG, "hex value");
    }
    return hexBytes(*parsed);
}

[[nodiscard]] std::array<std::byte, sha256ByteCount> sha256Bytes(
    const std::span<const std::byte> bytes)
{
    Sha256Hasher hasher;
    hasher.append(bytes);
    return hasher.finish();
}

[[nodiscard]] std::array<std::byte, sha256ByteCount> sha256File(
    const fs::path& path)
{
    UniqueHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
    {
        throwLastError("CreateFileW(SHA-256)");
    }

    Sha256Hasher hasher;
    std::array<std::byte, 64U * 1024U> buffer{};
    for (;;)
    {
        DWORD bytesRead = 0U;
        if (!ReadFile(
                file.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytesRead,
                nullptr))
        {
            throwLastError("ReadFile(SHA-256)");
        }
        if (bytesRead == 0U)
        {
            break;
        }
        hasher.append(std::span(buffer.data(), bytesRead));
    }
    return hasher.finish();
}

[[nodiscard]] std::string readStateFile(const fs::path& path)
{
    std::error_code sizeError;
    const std::uintmax_t fileSize = fs::file_size(path, sizeError);
    if (sizeError)
    {
        throw HResultError(
            HRESULT_FROM_WIN32(sizeError.value()),
            "filesystem::file_size(INSTALL-STATE.json)");
    }
    if (fileSize == 0U || fileSize > maximumStateBytes)
    {
        throw HResultError(E_INVALIDARG, "INSTALL-STATE.json size");
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        throw HResultError(
            HRESULT_FROM_WIN32(ERROR_OPEN_FAILED),
            "ifstream(INSTALL-STATE.json)");
    }
    std::string contents(static_cast<std::size_t>(fileSize), '\0');
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(contents.size()))
    {
        throw HResultError(
            HRESULT_FROM_WIN32(ERROR_READ_FAULT),
            "read(INSTALL-STATE.json)");
    }
    return contents;
}

[[nodiscard]] InstallState parseInstallState(const std::string_view contents)
{
    const JsonObject root = JsonObject::Parse(winrt::to_hstring(contents));
    const double schema = root.GetNamedNumber(L"schema");
    if (schema != 2.0)
    {
        throw HResultError(E_INVALIDARG, "INSTALL-STATE.json schema");
    }
    const auto stringValue = [&root](const wchar_t* name)
    {
        return winrt::to_string(root.GetNamedString(name));
    };

    InstallState state{};
    state.packageName = stringValue(L"packageName");
    state.applicationId = stringValue(L"applicationId");
    state.publisher = stringValue(L"publisher");
    state.packageVersion = stringValue(L"packageVersion");
    state.packageFullName = stringValue(L"packageFullName");
    state.packageFamilyName = stringValue(L"packageFamilyName");
    state.certificateThumbprint = stringValue(L"certificateThumbprint");
    state.certificateSha256 = stringValue(L"certificateSha256");
    state.externalLocation = stringValue(L"externalLocation");
    state.installedUserSid = stringValue(L"installedUserSid");
    state.hostFile = stringValue(L"hostFile");
    state.hostSha256 = stringValue(L"hostSha256");
    state.packageFile = stringValue(L"packageFile");
    state.packageSha256 = stringValue(L"packageSha256");

    const fs::path packageFile = fs::path(
        winrt::to_hstring(state.packageFile).c_str());
    if (state.packageName != "CialloKing.BaClickFxDesktop"
        || state.applicationId != "BaClickFxDesktop"
        || state.publisher != "CN=BaClickFx.Local"
        || state.hostFile != "ba-click-fx-desktop.exe"
        || packageFile.empty()
        || packageFile.has_parent_path()
        || packageFile.filename() != packageFile
        || packageFile.extension() != L".msix")
    {
        throw HResultError(E_INVALIDARG, "INSTALL-STATE.json identity");
    }
    if (!parseHex(state.hostSha256).has_value()
        || state.hostSha256.size() != sha256ByteCount * 2U
        || !parseHex(state.packageSha256).has_value()
        || state.packageSha256.size() != sha256ByteCount * 2U
        || !parseHex(state.certificateSha256).has_value()
        || state.certificateSha256.size() != sha256ByteCount * 2U
        || !parseHex(state.certificateThumbprint).has_value()
        || state.certificateThumbprint.size() != sha1ByteCount * 2U)
    {
        throw HResultError(E_INVALIDARG, "INSTALL-STATE.json hashes");
    }
    state.hostSha256 = canonicalHex(state.hostSha256);
    state.packageSha256 = canonicalHex(state.packageSha256);
    state.certificateSha256 = canonicalHex(state.certificateSha256);
    state.certificateThumbprint = canonicalHex(state.certificateThumbprint);
    return state;
}

[[nodiscard]] fs::path currentImagePath()
{
    constexpr DWORD maximumPathCharacters = 32'768U;
    std::vector<wchar_t> buffer(512U, L'\0');
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0U)
        {
            throwLastError("GetModuleFileNameW");
        }
        if (length < buffer.size())
        {
            return fs::path(std::wstring(buffer.data(), length));
        }
        if (buffer.size() >= maximumPathCharacters)
        {
            throw HResultError(
                HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
                "GetModuleFileNameW");
        }
        buffer.resize(buffer.size() * 2U, L'\0');
    }
}

[[nodiscard]] bool equivalentPaths(
    const fs::path& left,
    const fs::path& right,
    HRESULT& error) noexcept
{
    std::error_code pathError;
    const bool equivalent = fs::equivalent(left, right, pathError);
    if (pathError)
    {
        error = HRESULT_FROM_WIN32(pathError.value());
        return false;
    }
    error = S_OK;
    return equivalent;
}

[[nodiscard]] bool currentTokenCanModify(const fs::path& path)
{
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    const DWORD securityResult = GetNamedSecurityInfoW(
        const_cast<PWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION
            | GROUP_SECURITY_INFORMATION
            | DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &rawDescriptor);
    if (securityResult != ERROR_SUCCESS)
    {
        throw HResultError(
            HRESULT_FROM_WIN32(securityResult),
            "GetNamedSecurityInfoW");
    }
    const std::unique_ptr<void, decltype(&LocalFree)> descriptor(
        rawDescriptor,
        LocalFree);

    HANDLE rawPrimaryToken = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY | TOKEN_DUPLICATE,
            &rawPrimaryToken))
    {
        throwLastError("OpenProcessToken(AccessCheck)");
    }
    UniqueHandle primaryToken(rawPrimaryToken);
    HANDLE rawImpersonationToken = nullptr;
    if (!DuplicateToken(
            primaryToken.get(),
            SecurityImpersonation,
            &rawImpersonationToken))
    {
        throwLastError("DuplicateToken(AccessCheck)");
    }
    UniqueHandle impersonationToken(rawImpersonationToken);

    GENERIC_MAPPING mapping{
        FILE_GENERIC_READ,
        FILE_GENERIC_WRITE,
        FILE_GENERIC_EXECUTE,
        FILE_ALL_ACCESS};
    DWORD privilegeBytes = sizeof(PRIVILEGE_SET) + 16U * sizeof(LUID_AND_ATTRIBUTES);
    std::vector<std::byte> privilegeBuffer(privilegeBytes);
    DWORD grantedAccess = 0U;
    BOOL accessStatus = FALSE;
    BOOL checked = AccessCheck(
        descriptor.get(),
        impersonationToken.get(),
        MAXIMUM_ALLOWED,
        &mapping,
        reinterpret_cast<PPRIVILEGE_SET>(privilegeBuffer.data()),
        &privilegeBytes,
        &grantedAccess,
        &accessStatus);
    if (!checked && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        privilegeBuffer.resize(privilegeBytes);
        checked = AccessCheck(
            descriptor.get(),
            impersonationToken.get(),
            MAXIMUM_ALLOWED,
            &mapping,
            reinterpret_cast<PPRIVILEGE_SET>(privilegeBuffer.data()),
            &privilegeBytes,
            &grantedAccess,
            &accessStatus);
    }
    if (!checked)
    {
        throwLastError("AccessCheck(MAXIMUM_ALLOWED)");
    }
    if (!accessStatus)
    {
        return false;
    }
    constexpr DWORD mutationAccess = FILE_WRITE_DATA
        | FILE_APPEND_DATA
        | FILE_WRITE_EA
        | FILE_WRITE_ATTRIBUTES
        | FILE_DELETE_CHILD
        | DELETE
        | WRITE_DAC
        | WRITE_OWNER;
    return (grantedAccess & mutationAccess) != 0U;
}

[[nodiscard]] bool pathProtected(const fs::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        throwLastError("GetFileAttributesW(protected identity path)");
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return false;
    }
    return !currentTokenCanModify(path);
}

[[nodiscard]] std::string currentUserSid()
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
    {
        throwLastError("OpenProcessToken");
    }
    UniqueHandle token(rawToken);
    DWORD bytesRequired = 0U;
    static_cast<void>(GetTokenInformation(
        token.get(),
        TokenUser,
        nullptr,
        0U,
        &bytesRequired));
    if (bytesRequired == 0U)
    {
        throwLastError("GetTokenInformation(size)");
    }
    std::vector<std::byte> buffer(bytesRequired);
    if (!GetTokenInformation(
            token.get(),
            TokenUser,
            buffer.data(),
            bytesRequired,
            &bytesRequired))
    {
        throwLastError("GetTokenInformation(TokenUser)");
    }
    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidText))
    {
        throwLastError("ConvertSidToStringSidW");
    }
    const std::wstring value(sidText);
    LocalFree(sidText);
    return winrt::to_string(winrt::hstring(value));
}

[[nodiscard]] std::vector<std::byte> certificateProperty(
    PCCERT_CONTEXT certificate,
    const DWORD property)
{
    DWORD size = 0U;
    if (!CertGetCertificateContextProperty(certificate, property, nullptr, &size))
    {
        throwLastError("CertGetCertificateContextProperty(size)");
    }
    std::vector<std::byte> value(size);
    if (!CertGetCertificateContextProperty(
            certificate,
            property,
            value.data(),
            &size))
    {
        throwLastError("CertGetCertificateContextProperty(value)");
    }
    value.resize(size);
    return value;
}

[[nodiscard]] bool certificateSubjectMatches(
    PCCERT_CONTEXT certificate,
    const std::string_view expectedSubject)
{
    const winrt::hstring subject = winrt::to_hstring(expectedSubject);
    DWORD encodedSize = 0U;
    if (!CertStrToNameW(
            X509_ASN_ENCODING,
            subject.c_str(),
            CERT_X500_NAME_STR,
            nullptr,
            nullptr,
            &encodedSize,
            nullptr))
    {
        throwLastError("CertStrToNameW(size)");
    }
    std::vector<BYTE> encoded(encodedSize);
    if (!CertStrToNameW(
            X509_ASN_ENCODING,
            subject.c_str(),
            CERT_X500_NAME_STR,
            nullptr,
            encoded.data(),
            &encodedSize,
            nullptr))
    {
        throwLastError("CertStrToNameW(value)");
    }
    CERT_NAME_BLOB expectedName{};
    expectedName.cbData = encodedSize;
    expectedName.pbData = encoded.data();
    return CertCompareCertificateName(
        X509_ASN_ENCODING,
        &certificate->pCertInfo->Subject,
        &expectedName);
}

[[nodiscard]] bool certificateAllowsCodeSigning(PCCERT_CONTEXT certificate)
{
    DWORD size = 0U;
    if (!CertGetEnhancedKeyUsage(certificate, 0U, nullptr, &size))
    {
        return false;
    }
    std::vector<std::byte> buffer(size);
    auto* usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(buffer.data());
    if (!CertGetEnhancedKeyUsage(certificate, 0U, usage, &size))
    {
        return false;
    }
    constexpr std::string_view expectedOid = "1.3.6.1.5.5.7.3.3";
    for (DWORD index = 0U; index < usage->cUsageIdentifier; ++index)
    {
        if (usage->rgpszUsageIdentifier[index] != nullptr
            && expectedOid == usage->rgpszUsageIdentifier[index])
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] CertificateContext findCertificate(
    HCERTSTORE store,
    const std::span<const std::byte> sha1)
{
    CRYPT_HASH_BLOB hash{};
    hash.cbData = static_cast<DWORD>(sha1.size());
    hash.pbData = reinterpret_cast<BYTE*>(const_cast<std::byte*>(sha1.data()));
    return CertificateContext(CertFindCertificateInStore(
        store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        0U,
        CERT_FIND_HASH,
        &hash,
        nullptr));
}

[[nodiscard]] ExternalHostTrustStatus verifyPackageCertificate(
    const fs::path& packagePath,
    const InstallState& state,
    ExternalHostTrustResult& result,
    HRESULT& error)
{
    WinTrustState trust(packagePath);
    if (trust.result() != ERROR_SUCCESS)
    {
        error = static_cast<HRESULT>(trust.result());
        return ExternalHostTrustStatus::PackageSignatureInvalid;
    }
    PCCERT_CONTEXT signer = trust.signerCertificate();
    if (signer == nullptr)
    {
        error = TRUST_E_NOSIGNATURE;
        return ExternalHostTrustStatus::PackageSignatureInvalid;
    }

    const std::optional<std::vector<std::byte>> expectedThumbprint =
        parseHex(state.certificateThumbprint);
    if (!expectedThumbprint.has_value())
    {
        error = E_INVALIDARG;
        return ExternalHostTrustStatus::StateInvalid;
    }
    const std::vector<std::byte> signerThumbprint = certificateProperty(
        signer,
        CERT_SHA1_HASH_PROP_ID);
    const auto signerSha256 = sha256Bytes(std::span(
        reinterpret_cast<const std::byte*>(signer->pbCertEncoded),
        signer->cbCertEncoded));
    result.observedCertificateSha256 = hexBytes(signerSha256);
    if (signerThumbprint != *expectedThumbprint
        || result.observedCertificateSha256 != state.certificateSha256
        || !certificateSubjectMatches(signer, state.publisher))
    {
        error = E_ACCESSDENIED;
        return ExternalHostTrustStatus::SignerCertificateMismatch;
    }
    if (CertVerifyTimeValidity(nullptr, signer->pCertInfo) != 0
        || !certificateAllowsCodeSigning(signer))
    {
        error = CERT_E_EXPIRED;
        return ExternalHostTrustStatus::CertificateInvalid;
    }

    CertificateStore trustedPeople(L"TrustedPeople");
    CertificateContext trusted = findCertificate(
        trustedPeople.get(),
        *expectedThumbprint);
    if (trusted.get() == nullptr
        || trusted.get()->cbCertEncoded != signer->cbCertEncoded
        || std::memcmp(
               trusted.get()->pbCertEncoded,
               signer->pbCertEncoded,
               signer->cbCertEncoded) != 0)
    {
        error = CRYPT_E_NOT_FOUND;
        return ExternalHostTrustStatus::CertificateStoreMismatch;
    }

    CertificateStore privateStore(L"My");
    CertificateContext privateCertificate = findCertificate(
        privateStore.get(),
        *expectedThumbprint);
    if (privateCertificate.get() != nullptr)
    {
        // The one-use target-machine signing key must not survive Prepare.
        error = E_ACCESSDENIED;
        return ExternalHostTrustStatus::CertificateStoreMismatch;
    }
    error = S_OK;
    return ExternalHostTrustStatus::Trusted;
}

[[nodiscard]] HRESULT incompleteIdentityError(
    const PackageIdentityInfo& identity) noexcept
{
    const std::array errors{
        identity.fullNameError,
        identity.packagePathError,
        identity.familyNameError,
        identity.packageIdError,
        identity.applicationUserModelIdError,
        identity.stagedPathError,
        identity.effectiveExternalPathError};
    for (const DWORD error : errors)
    {
        if (error != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(error);
        }
    }
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
}

}

ExternalHostTrustResult queryExternalHostTrust(
    const PackageIdentityInfo& identity) noexcept
{
    ExternalHostTrustResult result{};
    try
    {
        if (!identity.present)
        {
            result.status = ExternalHostTrustStatus::NotPackaged;
            result.error = identity.fullNameError == ERROR_SUCCESS
                ? HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE)
                : HRESULT_FROM_WIN32(identity.fullNameError);
            return result;
        }
        if (!packageIdentityComplete(identity))
        {
            result.status = ExternalHostTrustStatus::IdentityIncomplete;
            result.error = incompleteIdentityError(identity);
            return result;
        }

        const fs::path executableRoot = executableDirectory();
        const fs::path statePath =
            executableRoot / L"Installer" / L"INSTALL-STATE.json";
        result.statePath = winrt::to_string(winrt::hstring(statePath.native()));
        std::error_code existsError;
        if (!fs::is_regular_file(statePath, existsError))
        {
            result.status = ExternalHostTrustStatus::StateMissing;
            result.error = existsError
                ? HRESULT_FROM_WIN32(existsError.value())
                : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            return result;
        }

        InstallState state{};
        try
        {
            state = parseInstallState(readStateFile(statePath));
        }
        catch (const HResultError& error)
        {
            result.status = ExternalHostTrustStatus::StateInvalid;
            result.error = error.result();
            return result;
        }
        catch (const winrt::hresult_error& error)
        {
            result.status = ExternalHostTrustStatus::StateInvalid;
            result.error = error.code();
            return result;
        }

        result.expectedHostSha256 = state.hostSha256;
        result.expectedPackageSha256 = state.packageSha256;
        result.expectedCertificateSha256 = state.certificateSha256;
        const std::string expectedAumid =
            state.packageFamilyName + "!" + state.applicationId;
        if (state.packageFullName != identity.fullName
            || state.packageFamilyName != identity.familyName
            || state.packageName != identity.name
            || state.publisher != identity.publisher
            || state.packageVersion != identity.version
            || expectedAumid != identity.applicationUserModelId)
        {
            result.status = ExternalHostTrustStatus::IdentityMismatch;
            result.error = E_ACCESSDENIED;
            return result;
        }
        if (state.installedUserSid != currentUserSid())
        {
            result.status = ExternalHostTrustStatus::UserMismatch;
            result.error = E_ACCESSDENIED;
            return result;
        }

        const fs::path externalLocation = fs::path(
            winrt::to_hstring(state.externalLocation).c_str());
        const fs::path effectiveExternal = fs::path(
            winrt::to_hstring(identity.effectiveExternalPath).c_str());
        HRESULT pathError = S_OK;
        if (!equivalentPaths(executableRoot, externalLocation, pathError)
            || !equivalentPaths(executableRoot, effectiveExternal, pathError))
        {
            result.status = ExternalHostTrustStatus::ExternalLocationMismatch;
            result.error = FAILED(pathError) ? pathError : E_ACCESSDENIED;
            return result;
        }

        const fs::path hostPath =
            externalLocation / winrt::to_hstring(state.hostFile).c_str();
        if (!equivalentPaths(currentImagePath(), hostPath, pathError))
        {
            result.status = ExternalHostTrustStatus::ImagePathMismatch;
            result.error = FAILED(pathError) ? pathError : E_ACCESSDENIED;
            return result;
        }

        const fs::path packagePath = externalLocation
            / L"Identity"
            / winrt::to_hstring(state.packageFile).c_str();
        const std::array protectedPaths{
            externalLocation,
            hostPath,
            externalLocation / L"Identity",
            packagePath,
            externalLocation / L"Installer",
            statePath};
        for (const fs::path& protectedPath : protectedPaths)
        {
            if (!pathProtected(protectedPath))
            {
                result.status = ExternalHostTrustStatus::PathUnprotected;
                result.error = E_ACCESSDENIED;
                return result;
            }
        }
        result.observedHostSha256 = hexBytes(sha256File(hostPath));
        if (result.observedHostSha256 != state.hostSha256)
        {
            result.status = ExternalHostTrustStatus::HostHashMismatch;
            result.error = TRUST_E_BAD_DIGEST;
            return result;
        }

        const fs::path stagedHostPath = fs::path(
            winrt::to_hstring(identity.stagedPath).c_str())
            / winrt::to_hstring(state.hostFile).c_str();
        if (hexBytes(sha256File(stagedHostPath)) != state.hostSha256)
        {
            result.status = ExternalHostTrustStatus::StagedHostHashMismatch;
            result.error = TRUST_E_BAD_DIGEST;
            return result;
        }

        result.observedPackageSha256 = hexBytes(sha256File(packagePath));
        if (result.observedPackageSha256 != state.packageSha256)
        {
            result.status = ExternalHostTrustStatus::PackageHashMismatch;
            result.error = TRUST_E_BAD_DIGEST;
            return result;
        }

        result.status = verifyPackageCertificate(
            packagePath,
            state,
            result,
            result.error);
        return result;
    }
    catch (const HResultError& error)
    {
        result.status = ExternalHostTrustStatus::Failed;
        result.error = error.result();
    }
    catch (const winrt::hresult_error& error)
    {
        result.status = ExternalHostTrustStatus::Failed;
        result.error = error.code();
    }
    catch (const std::bad_alloc&)
    {
        result.status = ExternalHostTrustStatus::Failed;
        result.error = E_OUTOFMEMORY;
    }
    catch (...)
    {
        result.status = ExternalHostTrustStatus::Failed;
        result.error = E_FAIL;
    }
    return result;
}

bool externalHostTrusted(const ExternalHostTrustResult& result) noexcept
{
    return result.status == ExternalHostTrustStatus::Trusted;
}

std::string_view externalHostTrustStatusName(
    const ExternalHostTrustStatus status) noexcept
{
    switch (status)
    {
    case ExternalHostTrustStatus::Trusted:
        return "trusted";
    case ExternalHostTrustStatus::NotPackaged:
        return "not-packaged";
    case ExternalHostTrustStatus::IdentityIncomplete:
        return "identity-incomplete";
    case ExternalHostTrustStatus::StateMissing:
        return "state-missing";
    case ExternalHostTrustStatus::StateInvalid:
        return "state-invalid";
    case ExternalHostTrustStatus::IdentityMismatch:
        return "identity-mismatch";
    case ExternalHostTrustStatus::UserMismatch:
        return "user-mismatch";
    case ExternalHostTrustStatus::ExternalLocationMismatch:
        return "external-location-mismatch";
    case ExternalHostTrustStatus::ImagePathMismatch:
        return "image-path-mismatch";
    case ExternalHostTrustStatus::PathUnprotected:
        return "path-unprotected";
    case ExternalHostTrustStatus::HostHashMismatch:
        return "host-hash-mismatch";
    case ExternalHostTrustStatus::StagedHostHashMismatch:
        return "staged-host-hash-mismatch";
    case ExternalHostTrustStatus::PackageHashMismatch:
        return "package-hash-mismatch";
    case ExternalHostTrustStatus::PackageSignatureInvalid:
        return "package-signature-invalid";
    case ExternalHostTrustStatus::SignerCertificateMismatch:
        return "signer-certificate-mismatch";
    case ExternalHostTrustStatus::CertificateStoreMismatch:
        return "certificate-store-mismatch";
    case ExternalHostTrustStatus::CertificateInvalid:
        return "certificate-invalid";
    case ExternalHostTrustStatus::Failed:
        return "failed";
    }
    return "unknown";
}

std::string externalHostTrustDiagnostic(const ExternalHostTrustResult& result)
{
    std::ostringstream stream;
    stream << "Package.ExternalHostTrust="
           << externalHostTrustStatusName(result.status)
           << ";HRESULT=" << hexHresult(result.error);
    if (!result.statePath.empty())
    {
        stream << ";StatePath=" << result.statePath;
    }
    if (!result.observedHostSha256.empty())
    {
        stream << ";HostSha256=" << result.observedHostSha256;
    }
    if (!result.observedPackageSha256.empty())
    {
        stream << ";PackageSha256=" << result.observedPackageSha256;
    }
    if (!result.observedCertificateSha256.empty())
    {
        stream << ";CertificateSha256="
               << result.observedCertificateSha256;
    }
    return stream.str();
}

}

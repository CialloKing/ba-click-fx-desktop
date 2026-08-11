#include "appx_signer.hpp"

#include <windows.h>

#include <ncrypt.h>
#include <softpub.h>
#include <unknwn.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bafx::identity_signer
{
namespace
{

// SignerSignEx2's ABI structures are documented by Microsoft but omitted from
// the Windows SDK headers. Keep the declarations private to this translation unit.
struct SignerFileInfo
{
    DWORD cbSize;
    LPCWSTR pwszFileName;
    HANDLE hFile;
};

struct SignerBlobInfo
{
    DWORD cbSize;
    GUID* pGuidSubject;
    DWORD cbBlob;
    BYTE* pbBlob;
    LPCWSTR pwszDisplayName;
};

struct SignerSubjectInfo
{
    DWORD cbSize;
    DWORD* pdwIndex;
    DWORD dwSubjectChoice;
    union
    {
        SignerFileInfo* pSignerFileInfo;
        SignerBlobInfo* pSignerBlobInfo;
    };
};

struct SignerAttrAuthcode
{
    DWORD cbSize;
    BOOL fCommercial;
    BOOL fIndividual;
    LPCWSTR pwszName;
    LPCWSTR pwszInfo;
};

struct SignerSignatureInfo
{
    DWORD cbSize;
    ALG_ID algidHash;
    DWORD dwAttrChoice;
    union
    {
        SignerAttrAuthcode* pAttrAuthcode;
    };
    PCRYPT_ATTRIBUTES psAuthenticated;
    PCRYPT_ATTRIBUTES psUnauthenticated;
};

struct SignerProviderInfo
{
    DWORD cbSize;
    LPCWSTR pwszProviderName;
    DWORD dwProviderType;
    DWORD dwKeySpec;
    DWORD dwPvkChoice;
    union
    {
        LPWSTR pwszPvkFileName;
        LPWSTR pwszKeyContainer;
    };
};

struct SignerSpcChainInfo
{
    DWORD cbSize;
    LPCWSTR pwszSpcFile;
    DWORD dwCertPolicy;
    HCERTSTORE hCertStore;
};

struct SignerCertStoreInfo
{
    DWORD cbSize;
    PCCERT_CONTEXT pSigningCert;
    DWORD dwCertPolicy;
    HCERTSTORE hCertStore;
};

struct SignerCert
{
    DWORD cbSize;
    DWORD dwCertChoice;
    union
    {
        LPCWSTR pwszSpcFile;
        SignerCertStoreInfo* pCertStoreInfo;
        SignerSpcChainInfo* pSpcChainInfo;
    };
    HWND hwnd;
};

struct SignerContext
{
    DWORD cbSize;
    DWORD cbBlob;
    BYTE* pbBlob;
};

struct SignerSignEx2Params
{
    DWORD dwFlags;
    SignerSubjectInfo* pSubjectInfo;
    SignerCert* pSigningCert;
    SignerSignatureInfo* pSignatureInfo;
    SignerProviderInfo* pProviderInfo;
    DWORD dwTimestampFlags;
    PCSTR pszAlgorithmOid;
    PCWSTR pwszTimestampURL;
    PCRYPT_ATTRIBUTES pCryptAttrs;
    void* pSipData;
    SignerContext** pSignerContext;
    void* pCryptoPolicy;
    void* pReserved;
};

struct AppxSipClientData
{
    SignerSignEx2Params* pSignerParams;
    IUnknown* pAppxSipState;
};

using SignerSignEx2Function = HRESULT(WINAPI*)(
    DWORD,
    SignerSubjectInfo*,
    SignerCert*,
    SignerSignatureInfo*,
    SignerProviderInfo*,
    DWORD,
    PCSTR,
    PCWSTR,
    PCRYPT_ATTRIBUTES,
    void*,
    SignerContext**,
    void*,
    void*);

constexpr DWORD signerSubjectFile = 0x01U;
constexpr DWORD signerNoAttribute = 0x00U;
constexpr DWORD signerCertStore = 0x02U;
constexpr DWORD signerCertPolicyChainNoRoot = 0x08U;
constexpr wchar_t requiredSubject[] = L"CN=BaClickFx.Local";

struct ModuleDeleter
{
    void operator()(const HINSTANCE module) const noexcept
    {
        if (module != nullptr)
        {
            FreeLibrary(module);
        }
    }
};

struct StoreDeleter
{
    void operator()(void* store) const noexcept
    {
        if (store != nullptr)
        {
            CertCloseStore(store, 0U);
        }
    }
};

struct CertificateDeleter
{
    void operator()(const CERT_CONTEXT* certificate) const noexcept
    {
        if (certificate != nullptr)
        {
            CertFreeCertificateContext(certificate);
        }
    }
};

using UniqueModule = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleDeleter>;
using UniqueStore = std::unique_ptr<void, StoreDeleter>;
using UniqueCertificate = std::unique_ptr<const CERT_CONTEXT, CertificateDeleter>;

[[nodiscard]] std::string errorCode(const unsigned long code)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << code;
    return stream.str();
}

[[noreturn]] void throwSystemError(const char* operation, const DWORD code)
{
    throw std::runtime_error(std::string(operation) + " failed with " + errorCode(code));
}

[[noreturn]] void throwLastError(const char* operation)
{
    throwSystemError(operation, GetLastError());
}

[[noreturn]] void throwResult(const char* operation, const HRESULT result)
{
    throw std::runtime_error(
        std::string(operation) + " failed with "
        + errorCode(static_cast<unsigned long>(result)));
}

[[nodiscard]] UniqueStore openCertificateStore()
{
    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE
            | CERT_STORE_OPEN_EXISTING_FLAG
            | CERT_STORE_READONLY_FLAG,
        L"My");
    if (store == nullptr)
    {
        throwLastError("Opening LocalMachine\\My");
    }
    return UniqueStore(store);
}

[[nodiscard]] UniqueCertificate findCertificate(
    const HCERTSTORE store,
    const std::array<std::uint8_t, sha1ThumbprintSize>& thumbprint)
{
    CRYPT_HASH_BLOB hash{};
    hash.cbData = static_cast<DWORD>(thumbprint.size());
    hash.pbData = const_cast<BYTE*>(thumbprint.data());
    PCCERT_CONTEXT certificate = CertFindCertificateInStore(
        store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        0,
        CERT_FIND_SHA1_HASH,
        &hash,
        nullptr);
    if (certificate == nullptr)
    {
        const DWORD code = GetLastError();
        if (code == CRYPT_E_NOT_FOUND)
        {
            throw std::runtime_error(
                "The exact certificate thumbprint was not found in LocalMachine\\My");
        }
        throwSystemError("Searching LocalMachine\\My", code);
    }
    return UniqueCertificate(certificate);
}

void requireExactSubject(const PCCERT_CONTEXT certificate)
{
    // New-SelfSignedCertificate uses UTF8String for the CN while
    // CertStrToNameW commonly emits PrintableString. Comparing the encoded
    // DER blobs would reject an otherwise identical subject, so normalize the
    // complete X.500 name through the platform formatter first.
    DWORD characterCount = CertNameToStrW(
        X509_ASN_ENCODING,
        &certificate->pCertInfo->Subject,
        CERT_X500_NAME_STR,
        nullptr,
        0U);
    if (characterCount == 0U)
    {
        throwLastError("Formatting the signing certificate subject");
    }
    std::wstring subject(characterCount, L'\0');
    if (CertNameToStrW(
            X509_ASN_ENCODING,
            &certificate->pCertInfo->Subject,
            CERT_X500_NAME_STR,
            subject.data(),
            characterCount) == 0U)
    {
        throwLastError("Formatting the signing certificate subject");
    }
    if (!subject.empty() && subject.back() == L'\0')
    {
        subject.pop_back();
    }
    if (subject != requiredSubject)
    {
        throw std::runtime_error("The signing certificate subject must be exactly CN=BaClickFx.Local");
    }
}

class AcquiredPrivateKey final
{
public:
    explicit AcquiredPrivateKey(const PCCERT_CONTEXT certificate)
    {
        // Scheme C creates CNG keys by default, while ALLOW preserves CSP compatibility.
        if (!CryptAcquireCertificatePrivateKey(
                certificate,
                CRYPT_ACQUIRE_SILENT_FLAG
                    | CRYPT_ACQUIRE_COMPARE_KEY_FLAG
                    | CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG,
                nullptr,
                &handle_,
                &keySpec_,
                &callerMustFree_))
        {
            throwLastError("Acquiring the certificate private key");
        }
    }

    ~AcquiredPrivateKey()
    {
        if (!callerMustFree_)
        {
            return;
        }
        if (keySpec_ == CERT_NCRYPT_KEY_SPEC)
        {
            NCryptFreeObject(static_cast<NCRYPT_HANDLE>(handle_));
        }
        else
        {
            CryptReleaseContext(static_cast<HCRYPTPROV>(handle_), 0U);
        }
    }

    AcquiredPrivateKey(const AcquiredPrivateKey&) = delete;
    AcquiredPrivateKey& operator=(const AcquiredPrivateKey&) = delete;

private:
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE handle_{0U};
    DWORD keySpec_{0U};
    BOOL callerMustFree_{FALSE};
};

[[nodiscard]] UniqueModule loadSignerModule()
{
    HMODULE module = LoadLibraryExW(
        L"MSSign32.dll",
        nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr)
    {
        throwLastError("Loading MSSign32.dll from System32");
    }
    return UniqueModule(module);
}

[[nodiscard]] SignerSignEx2Function loadSignerFunction(const HMODULE module)
{
    const FARPROC procedure = GetProcAddress(module, "SignerSignEx2");
    if (procedure == nullptr)
    {
        throwLastError("Resolving SignerSignEx2");
    }
    return reinterpret_cast<SignerSignEx2Function>(procedure);
}

void invokeSigner(
    const std::filesystem::path& packagePath,
    const PCCERT_CONTEXT certificate,
    const HCERTSTORE store)
{
    DWORD signerIndex = 0U;
    SignerFileInfo fileInfo{};
    fileInfo.cbSize = sizeof(fileInfo);
    fileInfo.pwszFileName = packagePath.c_str();

    SignerSubjectInfo subjectInfo{};
    subjectInfo.cbSize = sizeof(subjectInfo);
    subjectInfo.pdwIndex = &signerIndex;
    subjectInfo.dwSubjectChoice = signerSubjectFile;
    subjectInfo.pSignerFileInfo = &fileInfo;

    SignerCertStoreInfo certStoreInfo{};
    certStoreInfo.cbSize = sizeof(certStoreInfo);
    certStoreInfo.pSigningCert = certificate;
    certStoreInfo.dwCertPolicy = signerCertPolicyChainNoRoot;
    certStoreInfo.hCertStore = store;

    SignerCert signerCertificate{};
    signerCertificate.cbSize = sizeof(signerCertificate);
    signerCertificate.dwCertChoice = signerCertStore;
    signerCertificate.pCertStoreInfo = &certStoreInfo;

    SignerSignatureInfo signatureInfo{};
    signatureInfo.cbSize = sizeof(signatureInfo);
    signatureInfo.algidHash = CALG_SHA_256;
    signatureInfo.dwAttrChoice = signerNoAttribute;

    SignerSignEx2Params parameters{};
    parameters.pSubjectInfo = &subjectInfo;
    parameters.pSigningCert = &signerCertificate;
    parameters.pSignatureInfo = &signatureInfo;

    AppxSipClientData sipClientData{};
    sipClientData.pSignerParams = &parameters;
    parameters.pSipData = &sipClientData;

    const UniqueModule signerModule = loadSignerModule();
    const SignerSignEx2Function signer = loadSignerFunction(signerModule.get());
    const HRESULT result = signer(
        parameters.dwFlags,
        parameters.pSubjectInfo,
        parameters.pSigningCert,
        parameters.pSignatureInfo,
        parameters.pProviderInfo,
        parameters.dwTimestampFlags,
        parameters.pszAlgorithmOid,
        parameters.pwszTimestampURL,
        parameters.pCryptAttrs,
        parameters.pSipData,
        parameters.pSignerContext,
        parameters.pCryptoPolicy,
        parameters.pReserved);

    // AppxSip owns this state even when signing fails, so release it before throwing.
    if (sipClientData.pAppxSipState != nullptr)
    {
        sipClientData.pAppxSipState->Release();
        sipClientData.pAppxSipState = nullptr;
    }
    if (FAILED(result))
    {
        throwResult("SignerSignEx2", result);
    }
}

void verifyPackageSignature(const std::filesystem::path& packagePath)
{
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = packagePath.c_str();

    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(nullptr, &policy, &trustData);

    // The provider state must be closed regardless of the verification result.
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    static_cast<void>(WinVerifyTrust(nullptr, &policy, &trustData));
    if (result != ERROR_SUCCESS)
    {
        throw std::runtime_error(
            "WinVerifyTrust rejected the signed package with "
            + errorCode(static_cast<unsigned long>(result)));
    }
}

}

void signPackage(const Options& options)
{
    if (options.storeLocation != CertificateStoreLocation::LocalMachine)
    {
        throw std::invalid_argument("Only the LocalMachine certificate store is supported");
    }

    validatePackagePath(options.packagePath);
    const UniqueStore store = openCertificateStore();
    const UniqueCertificate certificate = findCertificate(store.get(), options.thumbprint);
    requireExactSubject(certificate.get());

    // Probe the key before modifying the package so missing ACLs fail cleanly.
    const AcquiredPrivateKey privateKey(certificate.get());
    invokeSigner(options.packagePath, certificate.get(), store.get());
    verifyPackageSignature(options.packagePath);
}

}

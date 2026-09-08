#include "package_activation.hpp"

#include "product/version.hpp"

#include <shobjidl_core.h>
#include <wrl/client.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace bafx::control_center
{
namespace
{

struct InstallStateFileRead final
{
    PackageActivationIdentityResult result{};
    std::string normalizedContents{};
    bool contentsRead{false};
};

constexpr std::size_t maximumInstallStateBytes = 64U * 1024U;
constexpr unsigned int expectedInstallStateSchema = 2U;
constexpr std::string_view expectedPackageFamilyPrefix =
    "CialloKing.BaClickFxDesktop_";
constexpr std::string_view expectedApplicationId = "BaClickFxDesktop";

[[nodiscard]] bool validHexMarker(
    const std::string_view value,
    const std::size_t expectedLength) noexcept
{
    if (value.size() != expectedLength)
    {
        return false;
    }
    for (const unsigned char character : value)
    {
        if (!std::isxdigit(character))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parseFixedDigits(
    const std::string_view value,
    const std::size_t offset,
    const std::size_t count,
    unsigned int& output) noexcept
{
    if (offset > value.size() || count > value.size() - offset)
    {
        return false;
    }

    unsigned int parsed = 0U;
    for (std::size_t index = 0U; index < count; ++index)
    {
        const unsigned char character =
            static_cast<unsigned char>(value[offset + index]);
        if (character < static_cast<unsigned char>('0')
            || character > static_cast<unsigned char>('9'))
        {
            return false;
        }
        parsed = parsed * 10U
            + static_cast<unsigned int>(character - static_cast<unsigned char>('0'));
    }
    output = parsed;
    return true;
}

[[nodiscard]] bool parseUtcFileTime(
    const std::string_view value,
    FILETIME& output) noexcept
{
    // The installer writes DateTime round-trip values in UTC. Keep this
    // parser deliberately strict so a malformed expiry cannot be treated as
    // an unexpired certificate by the UI.
    if (value.size() < 20U
        || value[4] != '-'
        || value[7] != '-'
        || value[10] != 'T'
        || value[13] != ':'
        || value[16] != ':')
    {
        return false;
    }

    const std::size_t lastIndex = value.size() - 1U;
    if (value[lastIndex] != 'Z')
    {
        return false;
    }

    unsigned int year = 0U;
    unsigned int month = 0U;
    unsigned int day = 0U;
    unsigned int hour = 0U;
    unsigned int minute = 0U;
    unsigned int second = 0U;
    if (!parseFixedDigits(value, 0U, 4U, year)
        || !parseFixedDigits(value, 5U, 2U, month)
        || !parseFixedDigits(value, 8U, 2U, day)
        || !parseFixedDigits(value, 11U, 2U, hour)
        || !parseFixedDigits(value, 14U, 2U, minute)
        || !parseFixedDigits(value, 17U, 2U, second))
    {
        return false;
    }

    std::uint64_t fractionalTicks = 0U;
    if (value[19] == '.')
    {
        unsigned int fractionValue = 0U;
        if (lastIndex <= 20U || lastIndex - 20U > 7U
            || !parseFixedDigits(
                value,
                20U,
                lastIndex - 20U,
                fractionValue))
        {
            return false;
        }
        fractionalTicks = fractionValue;
        for (std::size_t index = lastIndex - 20U; index < 7U; ++index)
        {
            fractionalTicks *= 10U;
        }
    }
    else if (value[19] != 'Z' || value.size() != 20U)
    {
        return false;
    }

    SYSTEMTIME systemTime{};
    systemTime.wYear = static_cast<WORD>(year);
    systemTime.wMonth = static_cast<WORD>(month);
    systemTime.wDay = static_cast<WORD>(day);
    systemTime.wHour = static_cast<WORD>(hour);
    systemTime.wMinute = static_cast<WORD>(minute);
    systemTime.wSecond = static_cast<WORD>(second);
    systemTime.wMilliseconds = static_cast<WORD>(fractionalTicks / 10'000U);
    FILETIME base{};
    if (SystemTimeToFileTime(&systemTime, &base) == FALSE)
    {
        return false;
    }

    ULARGE_INTEGER ticks{};
    ticks.LowPart = base.dwLowDateTime;
    ticks.HighPart = base.dwHighDateTime;
    const std::uint64_t remainder = fractionalTicks % 10'000U;
    if (ticks.QuadPart > (std::numeric_limits<std::uint64_t>::max)()
        - remainder)
    {
        return false;
    }
    ticks.QuadPart += remainder;
    output.dwLowDateTime = ticks.LowPart;
    output.dwHighDateTime = ticks.HighPart;
    return true;
}

[[nodiscard]] std::optional<PackageCertificateStatus>
certificateStatusFromTimestamp(const std::string_view value) noexcept
{
    if (value.empty())
    {
        return PackageCertificateStatus::Unknown;
    }

    FILETIME notAfter{};
    if (!parseUtcFileTime(value, notAfter))
    {
        return std::nullopt;
    }

    FILETIME nowFileTime{};
    GetSystemTimeAsFileTime(&nowFileTime);
    ULARGE_INTEGER now{};
    now.LowPart = nowFileTime.dwLowDateTime;
    now.HighPart = nowFileTime.dwHighDateTime;
    ULARGE_INTEGER expiry{};
    expiry.LowPart = notAfter.dwLowDateTime;
    expiry.HighPart = notAfter.dwHighDateTime;
    if (expiry.QuadPart <= now.QuadPart)
    {
        return PackageCertificateStatus::Expired;
    }

    constexpr std::uint64_t thirtyDays =
        30ULL * 24ULL * 60ULL * 60ULL * 10'000'000ULL;
    return expiry.QuadPart - now.QuadPart <= thirtyDays
        ? PackageCertificateStatus::ExpiringSoon
        : PackageCertificateStatus::Valid;
}

class InstallStateParser final
{
public:
    explicit InstallStateParser(const std::string_view input) noexcept
        : input_(input)
    {
    }

    [[nodiscard]] PackageActivationIdentityResult parse()
    {
        skipWhitespace();
        if (!consume('{'))
        {
            return fail(L"Install state must be a JSON object.");
        }

        std::optional<std::string> packageFamilyName;
        std::optional<std::string> applicationId;
        std::optional<std::string> productVersion;
        std::optional<std::string> packageVersion;
        std::optional<std::string> transactionId;
        std::optional<std::string> stateDigest;
        std::optional<std::string> certificateNotAfterUtc;
        bool hasSchema = false;
        unsigned int schema = 0U;

        skipWhitespace();
        if (consume('}'))
        {
            return fail(L"Install state is empty.");
        }

        while (position_ < input_.size())
        {
            skipWhitespace();
            const std::optional<std::string> key = parseString();
            if (!key.has_value())
            {
                return fail(L"Install state has an invalid property name.");
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return fail(L"Install state property is missing ':'.");
            }
            skipWhitespace();

            if (*key == "schema")
            {
                if (hasSchema || !parseUnsigned(schema))
                {
                    return fail(L"Install state schema must be one unsigned integer.");
                }
                hasSchema = true;
            }
            else if (*key == "packageFamilyName")
            {
                if (packageFamilyName.has_value())
                {
                    return fail(L"Install state repeats packageFamilyName.");
                }
                packageFamilyName = parseString();
                if (!packageFamilyName.has_value())
                {
                    return fail(L"Install state packageFamilyName must be a string.");
                }
            }
            else if (*key == "applicationId")
            {
                if (applicationId.has_value())
                {
                    return fail(L"Install state repeats applicationId.");
                }
                applicationId = parseString();
                if (!applicationId.has_value())
                {
                    return fail(L"Install state applicationId must be a string.");
                }
            }
            else if (*key == "productVersion")
            {
                if (productVersion.has_value())
                {
                    return fail(L"Install state repeats productVersion.");
                }
                productVersion = parseString();
                if (!productVersion.has_value())
                {
                    return fail(L"Install state productVersion must be a string.");
                }
            }
            else if (*key == "packageVersion")
            {
                if (packageVersion.has_value())
                {
                    return fail(L"Install state repeats packageVersion.");
                }
                packageVersion = parseString();
                if (!packageVersion.has_value())
                {
                    return fail(L"Install state packageVersion must be a string.");
                }
            }
            else if (*key == "transactionId")
            {
                if (transactionId.has_value())
                {
                    return fail(L"Install state repeats transactionId.");
                }
                transactionId = parseString();
                if (!transactionId.has_value())
                {
                    return fail(L"Install state transactionId must be a string.");
                }
            }
            else if (*key == "stateDigest")
            {
                if (stateDigest.has_value())
                {
                    return fail(L"Install state repeats stateDigest.");
                }
                stateDigest = parseString();
                if (!stateDigest.has_value())
                {
                    return fail(L"Install state stateDigest must be a string.");
                }
            }
            else if (*key == "certificateNotAfterUtc")
            {
                if (certificateNotAfterUtc.has_value())
                {
                    return fail(
                        L"Install state repeats certificateNotAfterUtc.");
                }
                certificateNotAfterUtc = parseString();
                if (!certificateNotAfterUtc.has_value())
                {
                    return fail(
                        L"Install state certificateNotAfterUtc must be a string.");
                }
            }
            else if (!skipPrimitive())
            {
                return fail(L"Install state has an unsupported property value.");
            }

            skipWhitespace();
            if (consume('}'))
            {
                break;
            }
            if (!consume(','))
            {
                return fail(L"Install state properties must be comma-separated.");
            }
        }

        skipWhitespace();
        if (position_ != input_.size())
        {
            return fail(L"Install state has trailing characters.");
        }
        if (!hasSchema)
        {
            return fail(L"Install state is missing schema.");
        }
        if (schema != expectedInstallStateSchema)
        {
            std::wstring message = L"Install state schema is unsupported; expected ";
            message += std::to_wstring(expectedInstallStateSchema);
            message += L", found ";
            message += std::to_wstring(schema);
            message += L".";
            return fail(message);
        }
        if (!packageFamilyName.has_value()
            || !applicationId.has_value()
            || !productVersion.has_value()
            || !packageVersion.has_value())
        {
            return fail(L"Install state is missing activation or version fields.");
        }
        if (!validPackageFamilyName(*packageFamilyName)
            || *applicationId != expectedApplicationId)
        {
            return fail(L"Install state package activation fields are invalid.");
        }
        if ((transactionId.has_value()
                && !validHexMarker(*transactionId, 32U))
            || (stateDigest.has_value()
                && !validHexMarker(*stateDigest, 64U)))
        {
            return fail(L"Install state transaction markers are invalid.");
        }

        std::wstring appUserModelId;
        appUserModelId.reserve(packageFamilyName->size() + applicationId->size() + 1U);
        for (const char value : *packageFamilyName)
        {
            appUserModelId.push_back(static_cast<wchar_t>(value));
        }
        appUserModelId.push_back(L'!');
        for (const char value : *applicationId)
        {
            appUserModelId.push_back(static_cast<wchar_t>(value));
        }

        const std::optional<bafx::product::VersionComponents> parsedProduct =
            bafx::product::parseProductVersion(*productVersion);
        const std::optional<bafx::product::VersionComponents> parsedPackage =
            bafx::product::parsePackageVersion(*packageVersion);
        if (!parsedProduct.has_value() || !parsedPackage.has_value())
        {
            return fail(L"Install state version fields are malformed.");
        }

        const std::optional<PackageCertificateStatus> certificateStatus =
            certificateStatusFromTimestamp(
                certificateNotAfterUtc.value_or(std::string{}));
        if (!certificateStatus.has_value())
        {
            return fail(L"Install state certificate expiry is malformed.");
        }

        PackageActivationIdentityResult result{};
        result.installStatePresent = true;
        result.certificateStatus = *certificateStatus;
        result.identity = PackageActivationIdentity{
            std::move(appUserModelId),
            std::move(*productVersion),
            std::move(*packageVersion),
            transactionId.value_or(std::string{}),
            stateDigest.value_or(std::string{}),
            certificateNotAfterUtc.value_or(std::string{})};
        if (*parsedProduct != *parsedPackage)
        {
            result.status = PackageActivationStateStatus::VersionMismatch;
            result.error =
                L"Install state productVersion and packageVersion disagree.";
            return result;
        }
        if (result.identity->productVersion != bafx::product::version)
        {
            result.status = PackageActivationStateStatus::PartialUpgrade;
            result.error =
                L"Install state and Control Center product versions differ; "
                L"the installation may be only partially upgraded.";
            return result;
        }
        result.status = PackageActivationStateStatus::Valid;
        return result;
    }

private:
    void skipWhitespace() noexcept
    {
        while (position_ < input_.size())
        {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
            {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool consume(const char expected) noexcept
    {
        if (position_ >= input_.size() || input_[position_] != expected)
        {
            return false;
        }
        ++position_;
        return true;
    }

    [[nodiscard]] bool consumeLiteral(const std::string_view literal) noexcept
    {
        if (input_.substr(position_, literal.size()) != literal)
        {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    [[nodiscard]] std::optional<std::string> parseString()
    {
        if (!consume('"'))
        {
            return std::nullopt;
        }

        std::string output;
        while (position_ < input_.size())
        {
            const unsigned char value =
                static_cast<unsigned char>(input_[position_++]);
            if (value == '"')
            {
                return output;
            }
            if (value < 0x20U)
            {
                return std::nullopt;
            }
            if (value != '\\')
            {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size())
            {
                return std::nullopt;
            }

            const char escape = input_[position_++];
            switch (escape)
            {
            case '"':
            case '\\':
            case '/':
                output.push_back(escape);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            default:
                // Installer-owned activation fields are ASCII. Rejecting
                // escaped UTF-16 also avoids accepting malformed surrogates.
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool parseUnsigned(unsigned int& output) noexcept
    {
        if (position_ >= input_.size()
            || input_[position_] < '0'
            || input_[position_] > '9')
        {
            return false;
        }

        unsigned int value = 0U;
        while (position_ < input_.size()
            && input_[position_] >= '0'
            && input_[position_] <= '9')
        {
            const unsigned int digit =
                static_cast<unsigned int>(input_[position_] - '0');
            if (value > ((std::numeric_limits<unsigned int>::max)() - digit) / 10U)
            {
                return false;
            }
            value = value * 10U + digit;
            ++position_;
        }
        output = value;
        return true;
    }

    [[nodiscard]] bool skipPrimitive()
    {
        if (position_ >= input_.size())
        {
            return false;
        }
        if (input_[position_] == '"')
        {
            return parseString().has_value();
        }
        if (consumeLiteral("true")
            || consumeLiteral("false")
            || consumeLiteral("null"))
        {
            return true;
        }

        const std::size_t begin = position_;
        if (input_[position_] == '-')
        {
            ++position_;
        }
        while (position_ < input_.size())
        {
            const char value = input_[position_];
            if (value == ',' || value == '}' || value == ' '
                || value == '\t' || value == '\r' || value == '\n')
            {
                break;
            }
            ++position_;
        }
        return position_ > begin;
    }

    [[nodiscard]] static bool validPackageFamilyName(
        const std::string_view value) noexcept
    {
        if (!value.starts_with(expectedPackageFamilyPrefix)
            || value.size() <= expectedPackageFamilyPrefix.size()
            || value.size() > 255U)
        {
            return false;
        }
        for (const unsigned char character : value)
        {
            if (std::isalnum(character) == 0
                && character != '.'
                && character != '_'
                && character != '-')
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] PackageActivationIdentityResult fail(
        const std::wstring_view message) const
    {
        PackageActivationIdentityResult result{};
        result.installStatePresent = true;
        result.status = PackageActivationStateStatus::Corrupt;
        result.error = std::wstring(message);
        return result;
    }

    std::string_view input_{};
    std::size_t position_{0U};
};

class ComApartment final
{
public:
    ComApartment() noexcept
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {
    }

    ~ComApartment()
    {
        if (result_ == S_OK || result_ == S_FALSE)
        {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] HRESULT result() const noexcept
    {
        return result_;
    }

private:
    HRESULT result_{E_FAIL};
};

}

PackageActivationIdentityResult parsePackageActivationState(
    const std::string_view json) noexcept
{
    try
    {
        return InstallStateParser(json).parse();
    }
    catch (...)
    {
        PackageActivationIdentityResult result{};
        result.installStatePresent = true;
        result.status = PackageActivationStateStatus::Corrupt;
        result.error = L"Install state could not be parsed due to an internal error.";
        return result;
    }
}

namespace
{

[[nodiscard]] InstallStateFileRead readPackageActivationStateFile(
    const std::filesystem::path& statePath) noexcept
{
    try
    {
        InstallStateFileRead file{};
        std::error_code error;
        const bool exists = std::filesystem::exists(statePath, error);
        if (error)
        {
            file.result.installStatePresent = true;
            file.result.status = PackageActivationStateStatus::Corrupt;
            file.result.error = L"The package install state could not be inspected.";
            return file;
        }
        if (!exists)
        {
            return file;
        }
        if (!std::filesystem::is_regular_file(statePath, error) || error)
        {
            file.result.installStatePresent = true;
            file.result.status = PackageActivationStateStatus::Corrupt;
            file.result.error = L"The package install state is not a regular file.";
            return file;
        }
        const std::uintmax_t size = std::filesystem::file_size(statePath, error);
        if (error || size == 0U || size > maximumInstallStateBytes)
        {
            file.result.installStatePresent = true;
            file.result.status = PackageActivationStateStatus::Corrupt;
            file.result.error = L"The package install state has an invalid size.";
            return file;
        }

        std::ifstream stream(statePath, std::ios::binary);
        if (!stream)
        {
            file.result.installStatePresent = true;
            file.result.status = PackageActivationStateStatus::Corrupt;
            file.result.error = L"The package install state could not be opened.";
            return file;
        }
        std::string contents(static_cast<std::size_t>(size), '\0');
        stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        const std::streamsize readCount = stream.gcount();
        const bool hasTrailingByte = stream.peek()
            != std::char_traits<char>::eof();
        if (readCount != static_cast<std::streamsize>(size)
            || stream.bad()
            || hasTrailingByte)
        {
            file.result.installStatePresent = true;
            file.result.status = PackageActivationStateStatus::Corrupt;
            file.result.error = L"The package install state could not be read completely.";
            return file;
        }
        if (contents.starts_with("\xEF\xBB\xBF"))
        {
            contents.erase(0U, 3U);
        }
        file.normalizedContents = contents;
        file.contentsRead = true;
        file.result = parsePackageActivationState(contents);
        return file;
    }
    catch (...)
    {
        InstallStateFileRead file{};
        file.result.installStatePresent = true;
        file.result.status = PackageActivationStateStatus::Corrupt;
        file.result.error = L"The package install state could not be loaded.";
        return file;
    }
}

}

PackageActivationIdentityResult readPackageActivationState(
    const std::filesystem::path& executableDirectory) noexcept
{
    const std::filesystem::path installerDirectory =
        executableDirectory / L"Installer";
    InstallStateFileRead primaryFile = readPackageActivationStateFile(
        installerDirectory / L"INSTALL-STATE.json");
    PackageActivationIdentityResult primary = std::move(primaryFile.result);
    primary.source = primary.installStatePresent
        ? PackageActivationStateSource::Primary
        : PackageActivationStateSource::None;
    InstallStateFileRead backupFile = readPackageActivationStateFile(
        installerDirectory / L"INSTALL-STATE.json.bak");
    PackageActivationIdentityResult backup = std::move(backupFile.result);
    backup.source = backup.installStatePresent
        ? PackageActivationStateSource::Backup
        : PackageActivationStateSource::None;
    const bool primaryValid = primary.status == PackageActivationStateStatus::Valid
        || primary.status == PackageActivationStateStatus::PartialUpgrade;
    const bool backupValid = backup.status == PackageActivationStateStatus::Valid
        || backup.status == PackageActivationStateStatus::PartialUpgrade;
    if (primaryValid && backupValid)
    {
        const bool sameTransaction = primary.identity.has_value()
            && backup.identity.has_value()
            && primary.identity->transactionId
                == backup.identity->transactionId
            && !primary.identity->transactionId.empty();
        const bool sameParsedIdentity = primary.identity.has_value()
            && backup.identity.has_value()
            && primary.identity->appUserModelId
                == backup.identity->appUserModelId
            && primary.identity->productVersion
                == backup.identity->productVersion
            && primary.identity->packageVersion
                == backup.identity->packageVersion;
        const bool sameDigest = primary.identity.has_value()
            && backup.identity.has_value()
            && !primary.identity->stateDigest.empty()
            && primary.identity->stateDigest
                == backup.identity->stateDigest;
        const bool bothLegacyWithoutDigest = primary.identity.has_value()
            && backup.identity.has_value()
            && primary.identity->stateDigest.empty()
            && backup.identity->stateDigest.empty();
        const bool sameModernBytes = primaryFile.contentsRead
            && backupFile.contentsRead
            && primaryFile.normalizedContents == backupFile.normalizedContents;
        const bool sameIdentity = sameTransaction
            && sameModernBytes
            && ((sameDigest) || (bothLegacyWithoutDigest && sameParsedIdentity));
        if (sameIdentity)
        {
            return primary;
        }
        primary.installStatePresent = true;
        primary.status = PackageActivationStateStatus::RepairRequired;
        primary.error =
            L"The install state and its backup belong to different transactions; "
            L"run the installer repair before starting Host.";
        return primary;
    }
    if (primary.status == PackageActivationStateStatus::Missing
        && backup.status == PackageActivationStateStatus::Missing)
    {
        return primary;
    }
    if (primaryValid || backupValid)
    {
        PackageActivationIdentityResult result = primaryValid ? primary : backup;
        result.installStatePresent = true;
        result.source = primaryValid
            ? PackageActivationStateSource::Primary
            : PackageActivationStateSource::Backup;
        result.status = PackageActivationStateStatus::RepairRequired;
        result.error =
            L"The protected install state is not a complete matching pair; "
            L"run the installer repair before starting Host.";
        return result;
    }
    if (primary.status == PackageActivationStateStatus::Corrupt
        && backup.status == PackageActivationStateStatus::Corrupt)
    {
        primary.error += L" Backup state is also invalid: ";
        primary.error += backup.error;
        return primary;
    }
    primary.installStatePresent = true;
    primary.status = PackageActivationStateStatus::RepairRequired;
    primary.error =
        L"The protected install state pair is incomplete; run the installer repair.";
    return primary;
}

PackageActivationResult activatePackagedHost(
    const std::wstring& appUserModelId) noexcept
{
    PackageActivationResult activation{};
    ComApartment apartment;
    if (FAILED(apartment.result()) && apartment.result() != RPC_E_CHANGED_MODE)
    {
        activation.result = apartment.result();
        return activation;
    }

    Microsoft::WRL::ComPtr<IApplicationActivationManager> manager;
    activation.result = CoCreateInstance(
        CLSID_ApplicationActivationManager,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&manager));
    if (FAILED(activation.result))
    {
        return activation;
    }

    activation.result = manager->ActivateApplication(
        appUserModelId.c_str(),
        nullptr,
        AO_NONE,
        &activation.processId);
    return activation;
}

}

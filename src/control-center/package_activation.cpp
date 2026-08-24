#include "package_activation.hpp"

#include "product/version.hpp"

#include <shobjidl_core.h>
#include <wrl/client.h>

#include <cctype>
#include <cstddef>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

namespace bafx::control_center
{
namespace
{

constexpr std::size_t maximumInstallStateBytes = 64U * 1024U;
constexpr unsigned int expectedInstallStateSchema = 2U;
constexpr std::string_view expectedPackageFamilyPrefix =
    "CialloKing.BaClickFxDesktop_";
constexpr std::string_view expectedApplicationId = "BaClickFxDesktop";

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

        PackageActivationIdentityResult result{};
        result.installStatePresent = true;
        result.identity = PackageActivationIdentity{
            std::move(appUserModelId),
            std::move(*productVersion),
            std::move(*packageVersion)};
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

[[nodiscard]] PackageActivationIdentityResult readPackageActivationStateFile(
    const std::filesystem::path& statePath) noexcept
{
    try
    {
        std::error_code error;
        const bool exists = std::filesystem::exists(statePath, error);
        if (error)
        {
            PackageActivationIdentityResult result{};
            result.installStatePresent = true;
            result.status = PackageActivationStateStatus::Corrupt;
            result.error = L"The package install state could not be inspected.";
            return result;
        }
        if (!exists)
        {
            return {};
        }
        if (!std::filesystem::is_regular_file(statePath, error) || error)
        {
            PackageActivationIdentityResult result{};
            result.installStatePresent = true;
            result.status = PackageActivationStateStatus::Corrupt;
            result.error = L"The package install state is not a regular file.";
            return result;
        }
        const std::uintmax_t size = std::filesystem::file_size(statePath, error);
        if (error || size == 0U || size > maximumInstallStateBytes)
        {
            PackageActivationIdentityResult result{};
            result.installStatePresent = true;
            result.status = PackageActivationStateStatus::Corrupt;
            result.error = L"The package install state has an invalid size.";
            return result;
        }

        std::ifstream stream(statePath, std::ios::binary);
        if (!stream)
        {
            PackageActivationIdentityResult result{};
            result.installStatePresent = true;
            result.status = PackageActivationStateStatus::Corrupt;
            result.error = L"The package install state could not be opened.";
            return result;
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
            PackageActivationIdentityResult result{};
            result.installStatePresent = true;
            result.status = PackageActivationStateStatus::Corrupt;
            result.error = L"The package install state could not be read completely.";
            return result;
        }
        if (contents.starts_with("\xEF\xBB\xBF"))
        {
            contents.erase(0U, 3U);
        }
        return parsePackageActivationState(contents);
    }
    catch (...)
    {
        PackageActivationIdentityResult result{};
        result.installStatePresent = true;
        result.status = PackageActivationStateStatus::Corrupt;
        result.error = L"The package install state could not be loaded.";
        return result;
    }
}

}

PackageActivationIdentityResult readPackageActivationState(
    const std::filesystem::path& executableDirectory) noexcept
{
    const std::filesystem::path installerDirectory =
        executableDirectory / L"Installer";
    PackageActivationIdentityResult primary = readPackageActivationStateFile(
        installerDirectory / L"INSTALL-STATE.json");
    primary.source = primary.installStatePresent
        ? PackageActivationStateSource::Primary
        : PackageActivationStateSource::None;
    if (primary.status != PackageActivationStateStatus::Corrupt
        && primary.status != PackageActivationStateStatus::Missing)
    {
        // A syntactically valid primary is authoritative. Falling back on a
        // stale backup would hide a real cross-version or partial upgrade.
        return primary;
    }

    PackageActivationIdentityResult backup = readPackageActivationStateFile(
        installerDirectory / L"INSTALL-STATE.json.bak");
    backup.source = backup.installStatePresent
        ? PackageActivationStateSource::Backup
        : PackageActivationStateSource::None;
    if (primary.status == PackageActivationStateStatus::Missing)
    {
        if (backup.status == PackageActivationStateStatus::Missing)
        {
            return primary;
        }
        backup.installStatePresent = true;
        backup.source = PackageActivationStateSource::Backup;
        if (backup.status == PackageActivationStateStatus::Valid
            || backup.status == PackageActivationStateStatus::PartialUpgrade)
        {
            backup.status = PackageActivationStateStatus::PartialUpgrade;
            backup.error =
                L"The primary install state is missing while its backup remains; "
                L"the installation may be only partially upgraded.";
        }
        return backup;
    }

    if (backup.status == PackageActivationStateStatus::Valid)
    {
        backup.status = PackageActivationStateStatus::BackupRecovered;
        backup.source = PackageActivationStateSource::Backup;
        return backup;
    }
    if (backup.status == PackageActivationStateStatus::Missing)
    {
        return primary;
    }
    if (backup.status == PackageActivationStateStatus::Corrupt)
    {
        primary.error += L" Backup state is also invalid: ";
        primary.error += backup.error;
        return primary;
    }

    backup.error = L"The primary install state is corrupt. " + backup.error;
    return backup;
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

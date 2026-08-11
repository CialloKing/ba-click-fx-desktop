#include "identity_signer_options.hpp"

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <string>

namespace bafx::identity_signer
{
namespace
{

[[nodiscard]] std::uint8_t parseHexDigit(const wchar_t character)
{
    if (character >= L'0' && character <= L'9')
    {
        return static_cast<std::uint8_t>(character - L'0');
    }
    if (character >= L'A' && character <= L'F')
    {
        return static_cast<std::uint8_t>(character - L'A' + 10);
    }
    if (character >= L'a' && character <= L'f')
    {
        return static_cast<std::uint8_t>(character - L'a' + 10);
    }
    throw std::invalid_argument("--thumbprint must contain exactly 40 hexadecimal characters");
}

[[nodiscard]] bool equalsCaseInsensitive(
    const std::wstring_view left,
    const std::wstring_view right)
{
    return left.size() == right.size()
        && std::equal(
            left.begin(),
            left.end(),
            right.begin(),
            [](const wchar_t first, const wchar_t second)
            {
                return std::towlower(first) == std::towlower(second);
            });
}

}

std::array<std::uint8_t, sha1ThumbprintSize> parseThumbprint(
    const std::wstring_view value)
{
    if (value.size() != sha1ThumbprintSize * 2U)
    {
        throw std::invalid_argument("--thumbprint must contain exactly 40 hexadecimal characters");
    }

    std::array<std::uint8_t, sha1ThumbprintSize> result{};
    for (std::size_t index = 0U; index < result.size(); ++index)
    {
        const std::uint8_t high = parseHexDigit(value[index * 2U]);
        const std::uint8_t low = parseHexDigit(value[index * 2U + 1U]);
        result[index] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return result;
}

Options parseOptions(const std::span<const std::wstring_view> arguments)
{
    if (arguments.size() != 6U)
    {
        throw std::invalid_argument(
            "Expected --package, --thumbprint, and --store-location exactly once");
    }

    Options options{};
    bool hasPackage = false;
    bool hasThumbprint = false;
    bool hasStoreLocation = false;
    for (std::size_t index = 0U; index < arguments.size(); index += 2U)
    {
        const std::wstring_view name = arguments[index];
        const std::wstring_view value = arguments[index + 1U];
        if (value.empty())
        {
            throw std::invalid_argument("Command-line option values must not be empty");
        }

        if (name == L"--package")
        {
            if (hasPackage)
            {
                throw std::invalid_argument("--package must be specified exactly once");
            }
            options.packagePath = std::wstring(value);
            hasPackage = true;
        }
        else if (name == L"--thumbprint")
        {
            if (hasThumbprint)
            {
                throw std::invalid_argument("--thumbprint must be specified exactly once");
            }
            options.thumbprint = parseThumbprint(value);
            hasThumbprint = true;
        }
        else if (name == L"--store-location")
        {
            if (hasStoreLocation)
            {
                throw std::invalid_argument("--store-location must be specified exactly once");
            }
            if (value != L"LocalMachine")
            {
                throw std::invalid_argument("--store-location must be LocalMachine");
            }
            options.storeLocation = CertificateStoreLocation::LocalMachine;
            hasStoreLocation = true;
        }
        else
        {
            throw std::invalid_argument("Unknown identity signer option");
        }
    }

    if (!hasPackage || !hasThumbprint || !hasStoreLocation)
    {
        throw std::invalid_argument(
            "Expected --package, --thumbprint, and --store-location exactly once");
    }
    return options;
}

void validatePackagePath(const std::filesystem::path& packagePath)
{
    if (!packagePath.is_absolute())
    {
        throw std::invalid_argument("--package must be an absolute path");
    }
    if (!equalsCaseInsensitive(packagePath.extension().wstring(), L".msix"))
    {
        throw std::invalid_argument("--package must name an .msix file");
    }

    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(packagePath, error);
    if (error)
    {
        throw std::runtime_error("Could not inspect --package: " + error.message());
    }
    if (!std::filesystem::is_regular_file(status))
    {
        throw std::invalid_argument("--package must name an existing regular file");
    }
}

}

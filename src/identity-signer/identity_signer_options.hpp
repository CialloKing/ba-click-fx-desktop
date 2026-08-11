#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace bafx::identity_signer
{

constexpr std::size_t sha1ThumbprintSize = 20U;

enum class CertificateStoreLocation
{
    LocalMachine
};

struct Options
{
    std::filesystem::path packagePath{};
    std::array<std::uint8_t, sha1ThumbprintSize> thumbprint{};
    CertificateStoreLocation storeLocation{CertificateStoreLocation::LocalMachine};
};

[[nodiscard]] std::array<std::uint8_t, sha1ThumbprintSize> parseThumbprint(
    std::wstring_view value);

[[nodiscard]] Options parseOptions(
    std::span<const std::wstring_view> arguments);

void validatePackagePath(const std::filesystem::path& packagePath);

}

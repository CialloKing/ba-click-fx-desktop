#pragma once

#include "bafx/windows/diagnostic_log.hpp"

#include <array>
#include <charconv>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bafx::desktop
{

// Own strings until append builds the string_view array. This keeps composed
// diagnostics independent of temporary formatting buffers.
class DiagnosticFields final
{
public:
    void add(const std::string_view key, const std::string_view value)
    {
        addOwned(std::string(key), std::string(value));
    }

    void add(const std::string_view key, const char* const value)
    {
        add(key, std::string_view(value != nullptr ? value : "<null>"));
    }

    void add(const std::string_view key, const std::uint64_t value)
    {
        add(key, std::to_string(value));
    }

    void add(const std::string_view key, const std::uint32_t value)
    {
        add(key, static_cast<std::uint64_t>(value));
    }

    void add(const std::string_view key, const std::int32_t value)
    {
        addOwned(std::string(key), std::to_string(value));
    }

    void add(const std::string_view key, const bool value)
    {
        add(key, std::string_view(value ? "true" : "false"));
    }

    void addDecimal(const std::string_view key, const double value)
    {
        std::array<char, 64U> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
            value, std::chars_format::fixed, 3);
        if (converted.ec != std::errc{})
        {
            throw std::length_error("Diagnostic decimal exceeds its bounded buffer");
        }
        add(key, std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }

    void addHex32(const std::string_view key, const std::uint32_t value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << "0x"
               << std::uppercase
               << std::hex
               << std::setw(8)
               << std::setfill('0')
               << value;
        add(key, stream.str());
    }

    void append(
        const std::filesystem::path& path,
        const std::string_view eventName,
        const bafx::windows::DiagnosticLevel level,
        bafx::windows::DiagnosticLogTiming* const timing = nullptr) const
    {
        std::vector<bafx::windows::DiagnosticField> views;
        views.reserve(fields_.size());
        for (const auto& [key, value] : fields_)
        {
            views.push_back(bafx::windows::DiagnosticField{key, value});
        }
        bafx::windows::appendDiagnosticEvent(path, eventName, views, level, timing);
    }

private:
    void addOwned(std::string key, std::string value)
    {
        fields_.emplace_back(std::move(key), std::move(value));
    }

    std::vector<std::pair<std::string, std::string>> fields_{};
};

}

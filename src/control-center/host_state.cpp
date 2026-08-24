#include "host_state.hpp"

#include "product/version.hpp"

#include <windows.h>

#include <charconv>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bafx::control_center
{
namespace
{

[[nodiscard]] std::optional<std::wstring> profileNameWide(
    const std::string_view value)
{
    if (value.empty()
        || value.size() > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)()))
    {
        return std::nullopt;
    }
    const int bytes = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        bytes,
        nullptr,
        0);
    if (required <= 0)
    {
        return std::nullopt;
    }
    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            bytes,
            converted.data(),
            required) != required)
    {
        return std::nullopt;
    }
    return converted;
}

[[nodiscard]] bool profileNamesEqual(
    const std::string_view left,
    const std::string_view right)
{
    if (left == right)
    {
        return true;
    }
    const std::optional<std::wstring> wideLeft = profileNameWide(left);
    const std::optional<std::wstring> wideRight = profileNameWide(right);
    if (!wideLeft.has_value() || !wideRight.has_value())
    {
        return false;
    }
    return CompareStringOrdinal(
               wideLeft->data(),
               static_cast<int>(wideLeft->size()),
               wideRight->data(),
               static_cast<int>(wideRight->size()),
               TRUE)
        == CSTR_EQUAL;
}

class StateJsonParser final
{
public:
    explicit StateJsonParser(const std::string_view input) noexcept
        : input_(input)
    {
    }

    [[nodiscard]] HostStateParseResult parse()
    {
        skipWhitespace();
        if (!consume('{'))
        {
            return fail("state must be a JSON object");
        }

        HostState state{};
        bool hasProductVersion = false;
        bool hasGeneration = false;
        bool hasPaused = false;
        bool hasBackgroundCapture = false;
        bool hasSpout2Enabled = false;
        bool hasSpout2Sender = false;
        bool hasSpout2Status = false;
        bool hasSpout2Error = false;
        bool hasSpout2OutputContract = false;
        bool hasFxProfileCatalog = false;
        bool hasActiveFxProfile = false;
        bool hasFxProfileWarning = false;

        skipWhitespace();
        if (consume('}'))
        {
            return fail("state object is empty");
        }

        while (position_ < input_.size())
        {
            skipWhitespace();
            const std::optional<std::string> key = parseString();
            if (!key.has_value())
            {
                return fail("state contains an invalid property name");
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return fail("state property is missing ':'");
            }
            skipWhitespace();

            if (*key == "productVersion")
            {
                if (hasProductVersion)
                {
                    return fail("productVersion must not be repeated");
                }
                hasProductVersion = true;
                if (position_ < input_.size() && input_[position_] == '"')
                {
                    std::optional<std::string> value = parseString();
                    if (!value.has_value())
                    {
                        return fail("productVersion string is malformed");
                    }
                    state.productVersion = std::move(*value);
                }
                else if (!skipPrimitive())
                {
                    return fail("productVersion has an unsupported value");
                }
            }
            else if (*key == "generation")
            {
                if (hasGeneration || !parseUnsigned(state.generation))
                {
                    return fail("generation must be one unsigned integer");
                }
                hasGeneration = true;
            }
            else if (*key == "paused")
            {
                if (hasPaused || !parseBoolean(state.paused))
                {
                    return fail("paused must be one boolean");
                }
                hasPaused = true;
            }
            else if (*key == "backgroundCapture")
            {
                std::optional<std::string> value = parseString();
                if (hasBackgroundCapture || !value.has_value())
                {
                    return fail("backgroundCapture must be one string");
                }
                state.backgroundCapture = std::move(*value);
                hasBackgroundCapture = true;
            }
            else if (*key == "spout2Enabled")
            {
                if (hasSpout2Enabled || !parseBoolean(state.spout2Enabled))
                {
                    return fail("spout2Enabled must be one boolean");
                }
                hasSpout2Enabled = true;
            }
            else if (*key == "spout2Sender")
            {
                std::optional<std::string> value = parseString();
                if (hasSpout2Sender || !value.has_value())
                {
                    return fail("spout2Sender must be one string");
                }
                state.spout2Sender = std::move(*value);
                hasSpout2Sender = true;
            }
            else if (*key == "spout2Status")
            {
                std::optional<std::string> value = parseString();
                if (hasSpout2Status || !value.has_value())
                {
                    return fail("spout2Status must be one string");
                }
                state.spout2Status = std::move(*value);
                hasSpout2Status = true;
            }
            else if (*key == "spout2Error")
            {
                std::optional<std::string> value = parseString();
                if (hasSpout2Error || !value.has_value())
                {
                    return fail("spout2Error must be one string");
                }
                state.spout2Error = std::move(*value);
                hasSpout2Error = true;
            }
            else if (*key == "spout2OutputContract")
            {
                std::optional<std::string> value = parseString();
                if (hasSpout2OutputContract || !value.has_value())
                {
                    return fail("spout2OutputContract must be one string");
                }
                state.spout2OutputContract = std::move(*value);
                hasSpout2OutputContract = true;
            }
            else if (*key == "fxProfileCatalog")
            {
                std::optional<std::string> value = parseString();
                if (hasFxProfileCatalog
                    || !value.has_value()
                    || !parseFxProfileCatalog(
                        *value,
                        state.fxProfiles))
                {
                    return fail("fxProfileCatalog must be one valid catalog");
                }
                hasFxProfileCatalog = true;
            }
            else if (*key == "activeFxProfile")
            {
                std::optional<std::string> value = parseString();
                if (hasActiveFxProfile
                    || !value.has_value()
                    || value->empty())
                {
                    return fail("activeFxProfile must be one non-empty string");
                }
                state.activeFxProfile = std::move(*value);
                hasActiveFxProfile = true;
            }
            else if (*key == "fxProfileWarning")
            {
                std::optional<std::string> value = parseString();
                if (hasFxProfileWarning || !value.has_value())
                {
                    return fail("fxProfileWarning must be one string");
                }
                state.fxProfileWarning = std::move(*value);
                hasFxProfileWarning = true;
            }
            else if (!skipPrimitive())
            {
                // GetState currently emits only primitives. Rejecting nested
                // additions keeps the accepted protocol surface explicit.
                return fail("state contains an unsupported property value");
            }

            skipWhitespace();
            if (consume('}'))
            {
                break;
            }
            if (!consume(','))
            {
                return fail("state properties must be comma-separated");
            }
        }

        skipWhitespace();
        if (position_ != input_.size())
        {
            return fail("state has trailing characters");
        }
        if (!hasGeneration || !hasPaused || !hasBackgroundCapture)
        {
            return fail("state is missing a required property");
        }
        if (hasProductVersion)
        {
            if (!state.productVersion.has_value()
                || !bafx::product::parseProductVersion(
                    *state.productVersion).has_value())
            {
                state.productVersionStatus =
                    HostProductVersionStatus::Invalid;
            }
            else if (*state.productVersion == bafx::product::version)
            {
                state.productVersionStatus = HostProductVersionStatus::Match;
            }
            else
            {
                state.productVersionStatus =
                    HostProductVersionStatus::Mismatch;
            }
        }
        const bool hasCompleteSpout2State = hasSpout2Enabled
            && hasSpout2Sender
            && hasSpout2Status
            && hasSpout2Error
            && hasSpout2OutputContract;
        if (!hasCompleteSpout2State)
        {
            return fail("state is missing the required Spout2 status group");
        }
        if (!hasFxProfileCatalog
            || !hasActiveFxProfile
            || !hasFxProfileWarning)
        {
            return fail("state is missing the required FX profile group");
        }
        bool activeProfileKnown = state.activeFxProfile == "自定义";
        for (const FxProfileState& profile : state.fxProfiles)
        {
            if (profileNamesEqual(profile.name, state.activeFxProfile))
            {
                activeProfileKnown = true;
                // Normalize to the catalog spelling so the combo box has one
                // stable identity for each Windows filename.
                state.activeFxProfile = profile.name;
                break;
            }
        }
        if (!activeProfileKnown)
        {
            return fail(
                "activeFxProfile must be custom or name a catalog profile");
        }

        HostStateParseResult result{};
        result.state = std::move(state);
        return result;
    }

private:
    [[nodiscard]] static bool parseFxProfileCatalog(
        const std::string_view catalog,
        std::vector<FxProfileState>& output)
    {
        constexpr std::size_t maximumProfileCount = 68U;
        output.clear();
        std::size_t begin = 0U;
        while (begin < catalog.size())
        {
            const std::size_t end = catalog.find('|', begin);
            const std::string_view item = catalog.substr(
                begin,
                end == std::string_view::npos
                    ? catalog.size() - begin
                    : end - begin);
            if (item.size() < 3U
                || item[1] != ':'
                || (item[0] != 'B' && item[0] != 'C')
                || output.size() >= maximumProfileCount)
            {
                return false;
            }
            const std::string_view name = item.substr(2U);
            if (name.empty())
            {
                return false;
            }
            for (const FxProfileState& existing : output)
            {
                if (profileNamesEqual(existing.name, name))
                {
                    return false;
                }
            }
            output.push_back(FxProfileState{
                std::string(name),
                item[0] == 'B'});
            if (end == std::string_view::npos)
            {
                return true;
            }
            if (end + 1U == catalog.size())
            {
                return false;
            }
            begin = end + 1U;
        }
        return !output.empty();
    }

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
            const unsigned char value = static_cast<unsigned char>(input_[position_++]);
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
                // Host state values are protocol tokens and never require
                // escaped UTF-16. Rejecting \u also avoids accepting malformed
                // surrogate pairs in this intentionally narrow parser.
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool parseUnsigned(std::uint64_t& output) noexcept
    {
        const std::size_t begin = position_;
        while (position_ < input_.size()
            && input_[position_] >= '0'
            && input_[position_] <= '9')
        {
            ++position_;
        }
        if (position_ == begin)
        {
            return false;
        }

        const char* first = input_.data() + begin;
        const char* last = input_.data() + position_;
        const auto parsed = std::from_chars(first, last, output);
        return parsed.ec == std::errc{} && parsed.ptr == last;
    }

    [[nodiscard]] bool parseBoolean(bool& output) noexcept
    {
        if (consumeLiteral("true"))
        {
            output = true;
            return true;
        }
        if (consumeLiteral("false"))
        {
            output = false;
            return true;
        }
        return false;
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
        bool ignoredBoolean = false;
        if (parseBoolean(ignoredBoolean) || consumeLiteral("null"))
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

    [[nodiscard]] HostStateParseResult fail(const std::string_view message) const
    {
        HostStateParseResult result{};
        result.error = std::string(message) + " at byte " + std::to_string(position_);
        return result;
    }

    std::string_view input_{};
    std::size_t position_{0U};
};

}

HostStateParseResult parseHostState(const std::string_view json) noexcept
{
    try
    {
        return StateJsonParser(json).parse();
    }
    catch (...)
    {
        HostStateParseResult result{};
        result.error = "state parser could not allocate memory";
        return result;
    }
}

}

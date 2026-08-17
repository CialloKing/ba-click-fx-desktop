#include "display_state.hpp"

#include <charconv>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace bafx::control_center
{
namespace
{

constexpr std::size_t maximumDocumentBytes = 256U * 1024U;
constexpr std::size_t maximumStringBytes = 16U * 1024U;
constexpr std::size_t maximumSessions = 64U;
constexpr std::uint64_t requiredSessionFields = (1ULL << 31U) - 1ULL;

class DisplayStateJsonParser final
{
public:
    explicit DisplayStateJsonParser(const std::string_view input) noexcept
        : input_(input)
    {
    }

    [[nodiscard]] DisplayStateParseResult parse()
    {
        if (input_.size() > maximumDocumentBytes)
        {
            return failure("display state exceeds the protocol size limit");
        }

        DisplayState state{};
        skipWhitespace();
        if (!consume('{'))
        {
            return failure("display state must be an object");
        }

        bool generationSeen = false;
        bool sessionsSeen = false;
        skipWhitespace();
        if (consume('}'))
        {
            return failure("display state object is empty");
        }

        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return failure("display state has an invalid property name");
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return failure("display state property is missing ':'");
            }
            skipWhitespace();

            if (key == "generation")
            {
                if (generationSeen || !parseUnsigned(state.generation))
                {
                    return failure(
                        "generation must be one unsigned integer");
                }
                generationSeen = true;
            }
            else if (key == "sessions")
            {
                if (sessionsSeen || !parseSessions(state.sessions))
                {
                    return failure(
                        "sessions must be one valid display array");
                }
                sessionsSeen = true;
            }
            else
            {
                return failure("display state contains an unknown property");
            }

            skipWhitespace();
            if (consume('}'))
            {
                break;
            }
            if (!consume(','))
            {
                return failure(
                    "display state properties must be comma-separated");
            }
            skipWhitespace();
        }

        skipWhitespace();
        if (position_ != input_.size())
        {
            return failure("display state has trailing characters");
        }
        if (!generationSeen || !sessionsSeen)
        {
            return failure("display state is missing a required property");
        }

        DisplayStateParseResult result{};
        result.state = std::move(state);
        return result;
    }

private:
    [[nodiscard]] bool parseSessions(
        std::vector<DisplaySessionState>& sessions)
    {
        if (!consume('['))
        {
            return false;
        }
        skipWhitespace();
        if (consume(']'))
        {
            return true;
        }

        while (position_ < input_.size())
        {
            if (sessions.size() >= maximumSessions)
            {
                return false;
            }
            DisplaySessionState session{};
            if (!parseSession(session))
            {
                return false;
            }
            sessions.push_back(std::move(session));
            skipWhitespace();
            if (consume(']'))
            {
                return true;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseSession(DisplaySessionState& session)
    {
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        std::uint64_t seen = 0U;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            if (!parseSessionField(key, session, seen))
            {
                return false;
            }
            skipWhitespace();
            if (consume('}'))
            {
                return seen == requiredSessionFields;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseSessionField(
        const std::string_view key,
        DisplaySessionState& session,
        std::uint64_t& seen)
    {
        if (key == "monitor")
        {
            return markField(seen, 0U) && parseString(session.monitor);
        }
        if (key == "device")
        {
            return markField(seen, 1U) && parseString(session.device);
        }
        if (key == "displayKey")
        {
            return markField(seen, 27U)
                && parseOptionalString(session.displayKey);
        }
        if (key == "coordinator")
        {
            return markField(seen, 2U) && parseBoolean(session.coordinator);
        }
        if (key == "primary")
        {
            return markField(seen, 3U) && parseBoolean(session.primary);
        }
        if (key == "effectsEnabled")
        {
            return markField(seen, 28U)
                && parseBoolean(session.effectsEnabled);
        }
        if (key == "hdrEnabled")
        {
            return markField(seen, 29U)
                && parseBoolean(session.hdrEnabled);
        }
        if (key == "framePacing")
        {
            return markField(seen, 30U)
                && parseFramePacing(session.framePacing);
        }
        if (key == "left")
        {
            return markField(seen, 4U) && parseSigned(session.left);
        }
        if (key == "top")
        {
            return markField(seen, 5U) && parseSigned(session.top);
        }
        if (key == "right")
        {
            return markField(seen, 6U) && parseSigned(session.right);
        }
        if (key == "bottom")
        {
            return markField(seen, 7U) && parseSigned(session.bottom);
        }
        if (key == "targetDpiX")
        {
            return markField(seen, 8U) && parseUnsigned(session.targetDpiX);
        }
        if (key == "targetDpiY")
        {
            return markField(seen, 9U) && parseUnsigned(session.targetDpiY);
        }
        if (key == "windowDpi")
        {
            return markField(seen, 10U) && parseUnsigned(session.windowDpi);
        }
        if (key == "displayRefresh")
        {
            return markField(seen, 11U)
                && parseRefreshRate(session.displayRefresh);
        }
        if (key == "captureRefresh")
        {
            return markField(seen, 12U)
                && parseRefreshRate(session.captureRefresh);
        }
        if (key == "adapter")
        {
            return markField(seen, 13U) && parseString(session.adapter);
        }
        if (key == "driver")
        {
            return markField(seen, 14U) && parseDriver(session.driver);
        }
        if (key == "requestedOutput")
        {
            return markField(seen, 15U)
                && parseOutput(session.requestedOutput);
        }
        if (key == "resolvedOutput")
        {
            return markField(seen, 16U)
                && parseOutput(session.resolvedOutput);
        }
        if (key == "actualOutput")
        {
            return markField(seen, 17U)
                && parseOutput(session.actualOutput);
        }
        if (key == "outputPolicySatisfied")
        {
            return markField(seen, 18U)
                && parseBoolean(session.outputPolicySatisfied);
        }
        if (key == "colorMode")
        {
            return markField(seen, 19U) && parseColor(session.colorMode);
        }
        if (key == "hdrSupported")
        {
            return markField(seen, 20U)
                && parseOptionalBoolean(session.hdrSupported);
        }
        if (key == "hdrActive")
        {
            return markField(seen, 21U)
                && parseOptionalBoolean(session.hdrActive);
        }
        if (key == "backgroundCaptureActive")
        {
            return markField(seen, 22U)
                && parseBoolean(session.backgroundCaptureActive);
        }
        if (key == "backgroundCaptureRestartAllowed")
        {
            return markField(seen, 23U)
                && parseBoolean(session.backgroundCaptureRestartAllowed);
        }
        if (key == "backgroundCaptureFailure")
        {
            return markField(seen, 24U)
                && parseString(session.backgroundCaptureFailure);
        }
        if (key == "renderFaulted")
        {
            return markField(seen, 25U)
                && parseBoolean(session.renderFaulted);
        }
        if (key == "outputContractFaulted")
        {
            return markField(seen, 26U)
                && parseBoolean(session.outputContractFaulted);
        }
        return false;
    }

    [[nodiscard]] bool parseRefreshRate(
        std::optional<DisplayRefreshState>& output)
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        DisplayRefreshState refresh{};
        bool numeratorSeen = false;
        bool denominatorSeen = false;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            if (key == "numerator")
            {
                if (numeratorSeen || !parseUnsigned(refresh.numerator))
                {
                    return false;
                }
                numeratorSeen = true;
            }
            else if (key == "denominator")
            {
                if (denominatorSeen || !parseUnsigned(refresh.denominator))
                {
                    return false;
                }
                denominatorSeen = true;
            }
            else
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                if (!numeratorSeen
                    || !denominatorSeen
                    || refresh.numerator == 0U
                    || refresh.denominator == 0U)
                {
                    return false;
                }
                output = refresh;
                return true;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseDriver(DisplayDriverState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "hardware")
        {
            output = DisplayDriverState::Hardware;
            return true;
        }
        if (token == "warp")
        {
            output = DisplayDriverState::Warp;
            return true;
        }
        if (token == "unknown")
        {
            output = DisplayDriverState::Unknown;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseOutput(DisplayOutputState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "conservative-sdr")
        {
            output = DisplayOutputState::ConservativeSdr;
            return true;
        }
        if (token == "linear-scrgb")
        {
            output = DisplayOutputState::LinearScRgb;
            return true;
        }
        if (token == "unknown")
        {
            output = DisplayOutputState::Unknown;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseFramePacing(
        bafx::config::FramePacing& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "match-display")
        {
            output = bafx::config::FramePacing::MatchDisplay;
            return true;
        }
        if (token == "60")
        {
            output = bafx::config::FramePacing::Fixed60;
            return true;
        }
        if (token == "120")
        {
            output = bafx::config::FramePacing::Fixed120;
            return true;
        }
        if (token == "144")
        {
            output = bafx::config::FramePacing::Fixed144;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseColor(DisplayColorState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "sdr")
        {
            output = DisplayColorState::Sdr;
            return true;
        }
        if (token == "wide-color-gamut")
        {
            output = DisplayColorState::WideColorGamut;
            return true;
        }
        if (token == "hdr")
        {
            output = DisplayColorState::Hdr;
            return true;
        }
        if (token == "unknown")
        {
            output = DisplayColorState::Unknown;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseOptionalBoolean(std::optional<bool>& output)
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        bool value = false;
        if (!parseBoolean(value))
        {
            return false;
        }
        output = value;
        return true;
    }

    [[nodiscard]] bool parseOptionalString(
        std::optional<std::string>& output)
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        std::string value;
        if (!parseString(value))
        {
            return false;
        }
        output = std::move(value);
        return true;
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

    template <typename Integer>
    [[nodiscard]] bool parseUnsigned(Integer& output) noexcept
    {
        const std::size_t begin = position_;
        if (!scanIntegerDigits())
        {
            return false;
        }

        std::uint64_t parsed = 0U;
        const auto result = std::from_chars(
            input_.data() + begin,
            input_.data() + position_,
            parsed);
        if (result.ec != std::errc{}
            || result.ptr != input_.data() + position_
            || parsed > static_cast<std::uint64_t>(
                (std::numeric_limits<Integer>::max)()))
        {
            return false;
        }
        output = static_cast<Integer>(parsed);
        return true;
    }

    [[nodiscard]] bool parseSigned(std::int32_t& output) noexcept
    {
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-')
        {
            ++position_;
        }
        if (!scanIntegerDigits())
        {
            return false;
        }

        std::int64_t parsed = 0;
        const auto result = std::from_chars(
            input_.data() + begin,
            input_.data() + position_,
            parsed);
        if (result.ec != std::errc{}
            || result.ptr != input_.data() + position_
            || parsed < (std::numeric_limits<std::int32_t>::min)()
            || parsed > (std::numeric_limits<std::int32_t>::max)())
        {
            return false;
        }
        output = static_cast<std::int32_t>(parsed);
        return true;
    }

    [[nodiscard]] bool scanIntegerDigits() noexcept
    {
        const std::size_t begin = position_;
        if (position_ >= input_.size()
            || input_[position_] < '0'
            || input_[position_] > '9')
        {
            return false;
        }
        if (input_[position_] == '0')
        {
            ++position_;
            return position_ >= input_.size()
                || input_[position_] < '0'
                || input_[position_] > '9';
        }
        while (position_ < input_.size()
            && input_[position_] >= '0'
            && input_[position_] <= '9')
        {
            ++position_;
        }
        return position_ > begin;
    }

    [[nodiscard]] bool parseString(std::string& output)
    {
        if (!consume('"'))
        {
            return false;
        }
        output.clear();
        while (position_ < input_.size())
        {
            const unsigned char character = static_cast<unsigned char>(
                input_[position_++]);
            if (character == '"')
            {
                return output.size() <= maximumStringBytes;
            }
            if (character < 0x20U)
            {
                return false;
            }
            if (character != '\\')
            {
                output.push_back(static_cast<char>(character));
            }
            else if (!parseEscape(output))
            {
                return false;
            }
            if (output.size() > maximumStringBytes)
            {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool parseEscape(std::string& output)
    {
        if (position_ >= input_.size())
        {
            return false;
        }
        const char escape = input_[position_++];
        switch (escape)
        {
        case '"':
        case '\\':
        case '/':
            output.push_back(escape);
            return true;
        case 'b':
            output.push_back('\b');
            return true;
        case 'f':
            output.push_back('\f');
            return true;
        case 'n':
            output.push_back('\n');
            return true;
        case 'r':
            output.push_back('\r');
            return true;
        case 't':
            output.push_back('\t');
            return true;
        case 'u':
            return parseUnicodeEscape(output);
        default:
            return false;
        }
    }

    [[nodiscard]] bool parseUnicodeEscape(std::string& output)
    {
        std::uint32_t codePoint = 0U;
        if (!parseHexQuad(codePoint))
        {
            return false;
        }
        if (codePoint >= 0xD800U && codePoint <= 0xDBFFU)
        {
            if (position_ + 2U > input_.size()
                || input_[position_] != '\\'
                || input_[position_ + 1U] != 'u')
            {
                return false;
            }
            position_ += 2U;
            std::uint32_t lowSurrogate = 0U;
            if (!parseHexQuad(lowSurrogate)
                || lowSurrogate < 0xDC00U
                || lowSurrogate > 0xDFFFU)
            {
                return false;
            }
            codePoint = 0x10000U
                + ((codePoint - 0xD800U) << 10U)
                + (lowSurrogate - 0xDC00U);
        }
        else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU)
        {
            return false;
        }
        appendUtf8(output, codePoint);
        return true;
    }

    [[nodiscard]] bool parseHexQuad(std::uint32_t& output) noexcept
    {
        if (position_ + 4U > input_.size())
        {
            return false;
        }
        output = 0U;
        for (std::size_t index = 0U; index < 4U; ++index)
        {
            const char character = input_[position_++];
            std::uint32_t nibble = 0U;
            if (character >= '0' && character <= '9')
            {
                nibble = static_cast<std::uint32_t>(character - '0');
            }
            else if (character >= 'a' && character <= 'f')
            {
                nibble = static_cast<std::uint32_t>(character - 'a') + 10U;
            }
            else if (character >= 'A' && character <= 'F')
            {
                nibble = static_cast<std::uint32_t>(character - 'A') + 10U;
            }
            else
            {
                return false;
            }
            output = (output << 4U) | nibble;
        }
        return true;
    }

    static void appendUtf8(std::string& output, const std::uint32_t codePoint)
    {
        if (codePoint <= 0x7FU)
        {
            output.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint <= 0x7FFU)
        {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        else if (codePoint <= 0xFFFFU)
        {
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        else
        {
            output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }

    [[nodiscard]] static bool markField(
        std::uint64_t& seen,
        const std::uint32_t field) noexcept
    {
        const std::uint64_t mask = 1ULL << field;
        if ((seen & mask) != 0U)
        {
            return false;
        }
        seen |= mask;
        return true;
    }

    void skipWhitespace() noexcept
    {
        while (position_ < input_.size())
        {
            const char character = input_[position_];
            if (character != ' '
                && character != '\t'
                && character != '\r'
                && character != '\n')
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

    [[nodiscard]] DisplayStateParseResult failure(
        const std::string_view message) const
    {
        DisplayStateParseResult result{};
        result.error = std::string(message)
            + " at byte " + std::to_string(position_);
        return result;
    }

    std::string_view input_{};
    std::size_t position_{0U};
};

}

DisplayStateParseResult parseDisplayState(const std::string_view json) noexcept
{
    try
    {
        return DisplayStateJsonParser(json).parse();
    }
    catch (...)
    {
        DisplayStateParseResult result{};
        result.error = "display state parser could not allocate memory";
        return result;
    }
}

}

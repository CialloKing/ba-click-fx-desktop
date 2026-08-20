#include "host_state.hpp"

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace bafx::control_center
{
namespace
{

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
        bool hasGeneration = false;
        bool hasPaused = false;
        bool hasBackgroundCapture = false;
        bool hasSpout2Enabled = false;
        bool hasSpout2Sender = false;
        bool hasSpout2Status = false;
        bool hasSpout2Error = false;
        bool hasSpout2OutputContract = false;

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

            if (*key == "generation")
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
        const bool hasCompleteSpout2State = hasSpout2Enabled
            && hasSpout2Sender
            && hasSpout2Status
            && hasSpout2Error
            && hasSpout2OutputContract;
        if (!hasCompleteSpout2State)
        {
            return fail("state is missing the required Spout2 status group");
        }

        HostStateParseResult result{};
        result.state = std::move(state);
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

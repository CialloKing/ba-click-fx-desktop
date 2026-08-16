#include "bafx/config/config.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdio>
#endif

namespace bafx::config
{
namespace
{

constexpr float referenceTrailWidthPixels = 2.7F;

struct JsonValue
{
    using Object = std::map<std::string, JsonValue, std::less<>>;
    using Array = std::vector<JsonValue>;
    using Storage = std::variant<
        std::nullptr_t,
        bool,
        double,
        std::string,
        Object,
        Array>;

    JsonValue() = default;
    explicit JsonValue(const std::nullptr_t value)
        : storage(value)
    {
    }
    explicit JsonValue(const bool value)
        : storage(value)
    {
    }
    explicit JsonValue(const double value)
        : storage(value)
    {
    }
    explicit JsonValue(std::string value)
        : storage(std::move(value))
    {
    }
    explicit JsonValue(Object value)
        : storage(std::move(value))
    {
    }
    explicit JsonValue(Array value)
        : storage(std::move(value))
    {
    }

    Storage storage{nullptr};
};

class JsonParser final
{
public:
    explicit JsonParser(const std::string_view input)
        : input_(input)
    {
    }

    [[nodiscard]] std::optional<JsonValue> parse()
    {
        skipWhitespace();
        std::optional<JsonValue> value = parseValue(0U);
        if (!value.has_value())
        {
            return std::nullopt;
        }
        skipWhitespace();
        if (position_ != input_.size())
        {
            fail("trailing characters after JSON value");
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] const std::string& error() const noexcept
    {
        return error_;
    }

private:
    static constexpr std::size_t maximumDepth = 64U;

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

    void fail(const std::string_view message) noexcept
    {
        if (error_.empty())
        {
            error_ = "JSON error at offset "
                + std::to_string(position_) + ": "
                + std::string(message);
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

    [[nodiscard]] std::optional<JsonValue> parseValue(const std::size_t depth)
    {
        if (depth > maximumDepth)
        {
            fail("nesting depth exceeds limit");
            return std::nullopt;
        }
        skipWhitespace();
        if (position_ >= input_.size())
        {
            fail("unexpected end of input");
            return std::nullopt;
        }

        switch (input_[position_])
        {
        case '{':
            return parseObject(depth + 1U);
        case '[':
            return parseArray(depth + 1U);
        case '"':
        {
            std::optional<std::string> value = parseString();
            if (!value.has_value())
            {
                return std::nullopt;
            }
            return JsonValue(std::move(*value));
        }
        case 't':
            if (consumeLiteral("true"))
            {
                return JsonValue(true);
            }
            break;
        case 'f':
            if (consumeLiteral("false"))
            {
                return JsonValue(false);
            }
            break;
        case 'n':
            if (consumeLiteral("null"))
            {
                return JsonValue(nullptr);
            }
            break;
        default:
            if (input_[position_] == '-' || input_[position_] >= '0'
                && input_[position_] <= '9')
            {
                return parseNumber();
            }
            break;
        }

        fail("invalid value");
        return std::nullopt;
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

    [[nodiscard]] std::optional<JsonValue> parseObject(const std::size_t depth)
    {
        if (!consume('{'))
        {
            fail("expected object");
            return std::nullopt;
        }

        JsonValue::Object object;
        skipWhitespace();
        if (consume('}'))
        {
            return JsonValue(std::move(object));
        }

        while (position_ < input_.size())
        {
            skipWhitespace();
            std::optional<std::string> key = parseString();
            if (!key.has_value())
            {
                return std::nullopt;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                fail("expected ':' after object key");
                return std::nullopt;
            }
            std::optional<JsonValue> value = parseValue(depth);
            if (!value.has_value())
            {
                return std::nullopt;
            }
            if (!object.emplace(std::move(*key), std::move(*value)).second)
            {
                fail("duplicate object key");
                return std::nullopt;
            }
            skipWhitespace();
            if (consume('}'))
            {
                return JsonValue(std::move(object));
            }
            if (!consume(','))
            {
                fail("expected ',' or '}' in object");
                return std::nullopt;
            }
        }

        fail("unterminated object");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<JsonValue> parseArray(const std::size_t depth)
    {
        if (!consume('['))
        {
            fail("expected array");
            return std::nullopt;
        }

        JsonValue::Array array;
        skipWhitespace();
        if (consume(']'))
        {
            return JsonValue(std::move(array));
        }

        while (position_ < input_.size())
        {
            std::optional<JsonValue> value = parseValue(depth);
            if (!value.has_value())
            {
                return std::nullopt;
            }
            array.push_back(std::move(*value));
            skipWhitespace();
            if (consume(']'))
            {
                return JsonValue(std::move(array));
            }
            if (!consume(','))
            {
                fail("expected ',' or ']' in array");
                return std::nullopt;
            }
        }

        fail("unterminated array");
        return std::nullopt;
    }

    [[nodiscard]] static int hexValue(const char value) noexcept
    {
        if (value >= '0' && value <= '9')
        {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f')
        {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F')
        {
            return value - 'A' + 10;
        }
        return -1;
    }

    [[nodiscard]] std::optional<std::uint32_t> parseUnicodeEscape()
    {
        if (!consume('u') || position_ + 4U > input_.size())
        {
            fail("invalid unicode escape");
            return std::nullopt;
        }
        std::uint32_t code = 0U;
        for (std::size_t index = 0U; index < 4U; ++index)
        {
            const int digit = hexValue(input_[position_++]);
            if (digit < 0)
            {
                fail("invalid unicode escape");
                return std::nullopt;
            }
            code = (code << 4U) | static_cast<std::uint32_t>(digit);
        }
        return code;
    }

    static void appendUtf8(std::string& output, const std::uint32_t code)
    {
        if (code <= 0x7FU)
        {
            output.push_back(static_cast<char>(code));
        }
        else if (code <= 0x7FFU)
        {
            output.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
        else if (code <= 0xFFFFU)
        {
            output.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
        else
        {
            output.push_back(static_cast<char>(0xF0U | (code >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
    }

    [[nodiscard]] std::optional<std::string> parseString()
    {
        if (!consume('"'))
        {
            fail("expected string");
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
                fail("control character in string");
                return std::nullopt;
            }
            if (value != '\\')
            {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size())
            {
                fail("unterminated escape");
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
            case 'u':
            {
                --position_;
                std::optional<std::uint32_t> code = parseUnicodeEscape();
                if (!code.has_value())
                {
                    return std::nullopt;
                }
                // Reject isolated surrogates rather than emitting invalid UTF-8.
                if (*code >= 0xD800U && *code <= 0xDFFFU)
                {
                    fail("isolated unicode surrogate");
                    return std::nullopt;
                }
                appendUtf8(output, *code);
                break;
            }
            default:
                fail("unknown string escape");
                return std::nullopt;
            }
        }

        fail("unterminated string");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<JsonValue> parseNumber()
    {
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-')
        {
            ++position_;
        }
        if (position_ >= input_.size())
        {
            fail("invalid number");
            return std::nullopt;
        }
        if (input_[position_] == '0')
        {
            ++position_;
            if (position_ < input_.size()
                && input_[position_] >= '0' && input_[position_] <= '9')
            {
                fail("leading zero in number");
                return std::nullopt;
            }
        }
        else if (input_[position_] >= '1' && input_[position_] <= '9')
        {
            while (position_ < input_.size()
                && input_[position_] >= '0' && input_[position_] <= '9')
            {
                ++position_;
            }
        }
        else
        {
            fail("invalid number");
            return std::nullopt;
        }

        if (position_ < input_.size() && input_[position_] == '.')
        {
            ++position_;
            const std::size_t fractionBegin = position_;
            while (position_ < input_.size()
                && input_[position_] >= '0' && input_[position_] <= '9')
            {
                ++position_;
            }
            if (fractionBegin == position_)
            {
                fail("fraction has no digits");
                return std::nullopt;
            }
        }

        if (position_ < input_.size()
            && (input_[position_] == 'e' || input_[position_] == 'E'))
        {
            ++position_;
            if (position_ < input_.size()
                && (input_[position_] == '+' || input_[position_] == '-'))
            {
                ++position_;
            }
            const std::size_t exponentBegin = position_;
            while (position_ < input_.size()
                && input_[position_] >= '0' && input_[position_] <= '9')
            {
                ++position_;
            }
            if (exponentBegin == position_)
            {
                fail("exponent has no digits");
                return std::nullopt;
            }
        }

        const char* numberBegin = input_.data() + begin;
        const char* numberEnd = input_.data() + position_;
        double number = 0.0;
        const auto parsed = std::from_chars(numberBegin, numberEnd, number);
        if (parsed.ec != std::errc{} || parsed.ptr != numberEnd
            || !std::isfinite(number))
        {
            fail("number is not finite");
            return std::nullopt;
        }
        return JsonValue(number);
    }

    std::string_view input_;
    std::size_t position_{0U};
    std::string error_{};
};

[[nodiscard]] const JsonValue::Object* objectOf(const JsonValue& value) noexcept
{
    return std::get_if<JsonValue::Object>(&value.storage);
}

[[nodiscard]] JsonValue::Object* objectOf(JsonValue& value) noexcept
{
    return std::get_if<JsonValue::Object>(&value.storage);
}

[[nodiscard]] const JsonValue* member(
    const JsonValue::Object& object,
    const std::string_view key) noexcept
{
    const auto iterator = object.find(key);
    return iterator == object.end() ? nullptr : &iterator->second;
}

[[nodiscard]] std::string qualifiedConfigName(
    const std::string_view section,
    const std::string_view key)
{
    if (section.empty())
    {
        return std::string(key);
    }
    return std::string(section) + "." + std::string(key);
}

[[nodiscard]] const JsonValue* requiredMember(
    const JsonValue::Object& object,
    const std::string_view key,
    const std::string_view section,
    std::string& error)
{
    const JsonValue* value = member(object, key);
    if (value == nullptr)
    {
        error = "config field '" + qualifiedConfigName(section, key)
            + "' is required by schema "
            + std::to_string(currentSchemaVersion);
    }
    return value;
}

[[nodiscard]] bool validateKnownMembers(
    const JsonValue::Object& object,
    const std::initializer_list<std::string_view> knownMembers,
    const std::string_view section,
    std::string& error)
{
    for (const auto& entry : object)
    {
        if (std::find(
                knownMembers.begin(),
                knownMembers.end(),
                std::string_view(entry.first)) != knownMembers.end())
        {
            continue;
        }

        // A schema bump must name every field. Accepting undeclared data would
        // make the current on-disk contract ambiguous.
        error = "config field '";
        if (!section.empty())
        {
            error += section;
            error += ".";
        }
        error += entry.first;
        error += "' is not supported by schema ";
        error += std::to_string(currentSchemaVersion);
        return false;
    }
    return true;
}

[[nodiscard]] bool readBool(
    const JsonValue::Object& object,
    const std::string_view key,
    const std::string_view section,
    bool& output,
    std::string& error)
{
    const JsonValue* value = requiredMember(object, key, section, error);
    if (value == nullptr)
    {
        return false;
    }
    if (const bool* parsed = std::get_if<bool>(&value->storage); parsed != nullptr)
    {
        output = *parsed;
        return true;
    }
    error = "config field '" + qualifiedConfigName(section, key)
        + "' must be boolean";
    return false;
}

[[nodiscard]] bool readFloat(
    const JsonValue::Object& object,
    const std::string_view key,
    const std::string_view section,
    float& output,
    std::string& error)
{
    const JsonValue* value = requiredMember(object, key, section, error);
    if (value == nullptr)
    {
        return false;
    }
    const double* parsed = std::get_if<double>(&value->storage);
    if (parsed == nullptr || !std::isfinite(*parsed)
        || *parsed < -(std::numeric_limits<float>::max)()
        || *parsed > (std::numeric_limits<float>::max)())
    {
        error = "config field '" + qualifiedConfigName(section, key)
            + "' must be a finite number";
        return false;
    }
    output = static_cast<float>(*parsed);
    return true;
}

[[nodiscard]] bool readUnsignedInteger(
    const JsonValue::Object& object,
    const std::string_view key,
    const std::string_view section,
    std::uint32_t& output,
    std::string& error)
{
    const JsonValue* value = requiredMember(object, key, section, error);
    if (value == nullptr)
    {
        return false;
    }
    const double* parsed = std::get_if<double>(&value->storage);
    if (parsed == nullptr || !std::isfinite(*parsed)
        || *parsed < 0.0
        || *parsed > static_cast<double>((std::numeric_limits<std::uint32_t>::max)())
        || std::floor(*parsed) != *parsed)
    {
        error = "config field '" + qualifiedConfigName(section, key)
            + "' must be an unsigned integer";
        return false;
    }
    output = static_cast<std::uint32_t>(*parsed);
    return true;
}

[[nodiscard]] bool readString(
    const JsonValue::Object& object,
    const std::string_view key,
    const std::string_view section,
    std::string& output,
    std::string& error)
{
    const JsonValue* value = requiredMember(object, key, section, error);
    if (value == nullptr)
    {
        return false;
    }
    const std::string* parsed = std::get_if<std::string>(&value->storage);
    if (parsed == nullptr)
    {
        error = "config field '" + qualifiedConfigName(section, key)
            + "' must be a string";
        return false;
    }
    output = *parsed;
    return true;
}

[[nodiscard]] bool readEnum(
    const JsonValue::Object& object,
    const std::string_view key,
    const std::string_view section,
    std::string& output,
    std::string& error)
{
    return readString(object, key, section, output, error);
}

[[nodiscard]] bool parseRenderMode(
    const std::string_view value,
    RenderMode& output) noexcept
{
    if (value == "background-aware")
    {
        output = RenderMode::BackgroundAware;
        return true;
    }
    if (value == "recording-compatible")
    {
        output = RenderMode::RecordingCompatible;
        return true;
    }
    if (value == "light-background")
    {
        output = RenderMode::LightBackground;
        return true;
    }
    return false;
}

[[nodiscard]] bool parseBloomQuality(
    const std::string_view value,
    BloomQuality& output) noexcept
{
    if (value == "low")
    {
        output = BloomQuality::Low;
        return true;
    }
    if (value == "medium")
    {
        output = BloomQuality::Medium;
        return true;
    }
    if (value == "high")
    {
        output = BloomQuality::High;
        return true;
    }
    if (value == "ultra")
    {
        output = BloomQuality::Ultra;
        return true;
    }
    return false;
}

[[nodiscard]] bool parseFramePacing(
    const std::string_view value,
    FramePacing& output) noexcept
{
    if (value == "match-display")
    {
        output = FramePacing::MatchDisplay;
        return true;
    }
    if (value == "60")
    {
        output = FramePacing::Fixed60;
        return true;
    }
    if (value == "120")
    {
        output = FramePacing::Fixed120;
        return true;
    }
    if (value == "144")
    {
        output = FramePacing::Fixed144;
        return true;
    }
    return false;
}

[[nodiscard]] bool readRequiredObject(
    const JsonValue::Object& parent,
    const std::string_view key,
    const JsonValue::Object*& output,
    std::string& error)
{
    const JsonValue* value = member(parent, key);
    if (value == nullptr)
    {
        error = "config section '" + std::string(key)
            + "' is required by schema "
            + std::to_string(currentSchemaVersion);
        return false;
    }
    output = objectOf(*value);
    if (output == nullptr)
    {
        error = "config section '" + std::string(key) + "' must be an object";
        return false;
    }
    return true;
}

[[nodiscard]] Config parseCurrentConfig(
    const JsonValue::Object& root,
    std::string& error)
{
    Config config = defaultConfig();
    config.schemaVersion = currentSchemaVersion;

    if (!validateKnownMembers(
            root,
            {
                "schemaVersion",
                "effects",
                "background",
                "display",
                "input",
                "performance",
                "system"},
            {},
            error))
    {
        return config;
    }

    const JsonValue::Object* effects = nullptr;
    const JsonValue::Object* background = nullptr;
    const JsonValue::Object* display = nullptr;
    const JsonValue::Object* input = nullptr;
    const JsonValue::Object* performance = nullptr;
    const JsonValue::Object* system = nullptr;
    if (!readRequiredObject(root, "effects", effects, error)
        || !readRequiredObject(root, "background", background, error)
        || !readRequiredObject(root, "display", display, error)
        || !readRequiredObject(root, "input", input, error)
        || !readRequiredObject(root, "performance", performance, error)
        || !readRequiredObject(root, "system", system, error))
    {
        return config;
    }

    if (!validateKnownMembers(
                *effects,
                {
                    "enabled",
                    "globalScale",
                    "opacity",
                    "clickEnabled",
                    "trailEnabled",
                    "trailLength",
                    "trailWidth",
                    "clickTimeScale",
                    "trailTimeScale",
                    "trailLifetimeMs",
                    "diskLifetimeMs",
                    "diskRadius",
                    "ringsCount",
                    "ringsLifetimeMs",
                    "ringsRadiusMin",
                    "ringsRadiusMax",
                    "ringsAngularVelocityMultiplier",
                    "ringsRotationDirection",
                    "ringsHdrIntensity",
                    "shardsHdrIntensity",
                    "trailOpacity",
                    "bloomIntensity",
                    "bloomDiffusion",
                    "bloomThreshold",
                    "bloomSoftKnee",
                    "bloomClamp"},
                "effects",
                error)
        || !validateKnownMembers(
                *background,
                {"mode", "cursorExcluded", "allowSystemBorder"},
                "background",
                error)
        || !validateKnownMembers(
                *display,
                {"hdrEnabled"},
                "display",
                error)
        || !validateKnownMembers(
                *input,
                {
                    "leftClick",
                    "rightClick",
                    "middleClick",
                    "trailOnlyWhilePressed",
                    "samplingRateHz"},
                "input",
                error)
        || !validateKnownMembers(
                *performance,
                {"idleOptimization", "framePacing"},
                "performance",
                error)
        || !validateKnownMembers(
                *system,
                {"startWithWindows", "startMinimized", "closeToTray"},
                "system",
                error))
    {
        return config;
    }

    if (!readBool(*effects, "enabled", "effects", config.effects.enabled, error)
        || !readFloat(
            *effects,
            "globalScale",
            "effects",
            config.effects.globalScale,
            error)
        || !readFloat(
            *effects,
            "opacity",
            "effects",
            config.effects.opacity,
            error)
        || !readBool(
            *effects,
            "clickEnabled",
            "effects",
            config.effects.clickEnabled,
            error)
        || !readBool(
            *effects,
            "trailEnabled",
            "effects",
            config.effects.trailEnabled,
            error)
        || !readFloat(
            *effects,
            "trailLength",
            "effects",
            config.effects.trailLength,
            error)
        || !readFloat(
            *effects,
            "trailWidth",
            "effects",
            config.effects.trailWidth,
            error)
        || !readFloat(
            *effects,
            "clickTimeScale",
            "effects",
            config.effects.clickTimeScale,
            error)
        || !readFloat(
            *effects,
            "trailTimeScale",
            "effects",
            config.effects.trailTimeScale,
            error)
        || !readFloat(
            *effects,
            "trailLifetimeMs",
            "effects",
            config.effects.trailLifetimeMs,
            error)
        || !readFloat(
            *effects,
            "diskLifetimeMs",
            "effects",
            config.effects.diskLifetimeMs,
            error)
        || !readFloat(
            *effects,
            "diskRadius",
            "effects",
            config.effects.diskRadius,
            error)
        || !readUnsignedInteger(
            *effects,
            "ringsCount",
            "effects",
            config.effects.ringsCount,
            error)
        || !readFloat(
            *effects,
            "ringsLifetimeMs",
            "effects",
            config.effects.ringsLifetimeMs,
            error)
        || !readFloat(
            *effects,
            "ringsRadiusMin",
            "effects",
            config.effects.ringsRadiusMin,
            error)
        || !readFloat(
            *effects,
            "ringsRadiusMax",
            "effects",
            config.effects.ringsRadiusMax,
            error)
        || !readFloat(
            *effects,
            "ringsAngularVelocityMultiplier",
            "effects",
            config.effects.ringsAngularVelocityMultiplier,
            error)
        || !readFloat(
            *effects,
            "ringsRotationDirection",
            "effects",
            config.effects.ringsRotationDirection,
            error)
        || !readFloat(
            *effects,
            "ringsHdrIntensity",
            "effects",
            config.effects.ringsHdrIntensity,
            error)
        || !readFloat(
            *effects,
            "shardsHdrIntensity",
            "effects",
            config.effects.shardsHdrIntensity,
            error)
        || !readFloat(
            *effects,
            "trailOpacity",
            "effects",
            config.effects.trailOpacity,
            error)
        || !readFloat(
            *effects,
            "bloomIntensity",
            "effects",
            config.effects.bloomIntensity,
            error)
        || !readFloat(
            *effects,
            "bloomDiffusion",
            "effects",
            config.effects.bloomDiffusion,
            error)
        || !readFloat(
            *effects,
            "bloomThreshold",
            "effects",
            config.effects.bloomThreshold,
            error)
        || !readFloat(
            *effects,
            "bloomSoftKnee",
            "effects",
            config.effects.bloomSoftKnee,
            error)
        || !readFloat(
            *effects,
            "bloomClamp",
            "effects",
            config.effects.bloomClamp,
            error))
    {
        return config;
    }
    // trailLifetimeMs is the Web-facing source of truth. The legacy compact
    // multiplier is serialized as a derived convenience value only.
    config.effects.trailLength = config.effects.trailLifetimeMs / 300.0F;

    if (!readBool(
            *background,
            "cursorExcluded",
            "background",
            config.background.cursorExcluded,
            error)
        || !readBool(
            *background,
            "allowSystemBorder",
            "background",
            config.background.allowSystemBorder,
            error))
    {
        return config;
    }
    std::string mode;
    if (!readEnum(*background, "mode", "background", mode, error))
    {
        return config;
    }
    if (!parseRenderMode(mode, config.background.mode))
    {
        error = "config field 'background.mode' has an unknown value";
        return config;
    }

    if (!readBool(
            *display,
            "hdrEnabled",
            "display",
            config.display.hdrEnabled,
            error))
    {
        return config;
    }

    if (!readBool(*input, "leftClick", "input", config.input.leftClick, error)
        || !readBool(*input, "rightClick", "input", config.input.rightClick, error)
        || !readBool(*input, "middleClick", "input", config.input.middleClick, error)
        || !readBool(
            *input,
            "trailOnlyWhilePressed",
            "input",
            config.input.trailOnlyWhilePressed,
            error)
        || !readUnsignedInteger(
            *input,
            "samplingRateHz",
            "input",
            config.input.samplingRateHz,
            error))
    {
        return config;
    }

    if (!readBool(
            *performance,
            "idleOptimization",
            "performance",
            config.performance.idleOptimization,
            error))
    {
        return config;
    }
    std::string pacing;
    if (!readEnum(
            *performance,
            "framePacing",
            "performance",
            pacing,
            error))
    {
        return config;
    }
    if (!parseFramePacing(pacing, config.performance.framePacing))
    {
        error = "config field 'performance.framePacing' has an unknown value";
        return config;
    }

    if (!readBool(
            *system,
            "startWithWindows",
            "system",
            config.system.startWithWindows,
            error)
        || !readBool(
            *system,
            "startMinimized",
            "system",
            config.system.startMinimized,
            error)
        || !readBool(
            *system,
            "closeToTray",
            "system",
            config.system.closeToTray,
            error))
    {
        return config;
    }

    return config;
}

[[nodiscard]] JsonValue makeConfigJson(const Config& config)
{
    JsonValue::Object effects;
    effects.emplace("bloomIntensity", JsonValue(static_cast<double>(config.effects.bloomIntensity)));
    effects.emplace("clickEnabled", JsonValue(config.effects.clickEnabled));
    effects.emplace("clickTimeScale", JsonValue(static_cast<double>(config.effects.clickTimeScale)));
    effects.emplace("diskLifetimeMs", JsonValue(static_cast<double>(config.effects.diskLifetimeMs)));
    effects.emplace("diskRadius", JsonValue(static_cast<double>(config.effects.diskRadius)));
    effects.emplace("enabled", JsonValue(config.effects.enabled));
    effects.emplace("globalScale", JsonValue(static_cast<double>(config.effects.globalScale)));
    effects.emplace("opacity", JsonValue(static_cast<double>(config.effects.opacity)));
    effects.emplace(
        "ringsAngularVelocityMultiplier",
        JsonValue(static_cast<double>(config.effects.ringsAngularVelocityMultiplier)));
    effects.emplace("ringsCount", JsonValue(static_cast<double>(config.effects.ringsCount)));
    effects.emplace("ringsHdrIntensity", JsonValue(static_cast<double>(config.effects.ringsHdrIntensity)));
    effects.emplace("ringsLifetimeMs", JsonValue(static_cast<double>(config.effects.ringsLifetimeMs)));
    effects.emplace("ringsRadiusMax", JsonValue(static_cast<double>(config.effects.ringsRadiusMax)));
    effects.emplace("ringsRadiusMin", JsonValue(static_cast<double>(config.effects.ringsRadiusMin)));
    effects.emplace(
        "ringsRotationDirection",
        JsonValue(static_cast<double>(config.effects.ringsRotationDirection)));
    effects.emplace("shardsHdrIntensity", JsonValue(static_cast<double>(config.effects.shardsHdrIntensity)));
    effects.emplace("trailEnabled", JsonValue(config.effects.trailEnabled));
    effects.emplace("trailLength", JsonValue(static_cast<double>(config.effects.trailLength)));
    effects.emplace("trailLifetimeMs", JsonValue(static_cast<double>(config.effects.trailLifetimeMs)));
    effects.emplace("trailOpacity", JsonValue(static_cast<double>(config.effects.trailOpacity)));
    effects.emplace("trailTimeScale", JsonValue(static_cast<double>(config.effects.trailTimeScale)));
    effects.emplace("trailWidth", JsonValue(static_cast<double>(config.effects.trailWidth)));
    effects.emplace("bloomClamp", JsonValue(static_cast<double>(config.effects.bloomClamp)));
    effects.emplace("bloomDiffusion", JsonValue(static_cast<double>(config.effects.bloomDiffusion)));
    effects.emplace("bloomSoftKnee", JsonValue(static_cast<double>(config.effects.bloomSoftKnee)));
    effects.emplace("bloomThreshold", JsonValue(static_cast<double>(config.effects.bloomThreshold)));

    JsonValue::Object background;
    background.emplace("allowSystemBorder", JsonValue(config.background.allowSystemBorder));
    background.emplace("cursorExcluded", JsonValue(config.background.cursorExcluded));
    background.emplace("mode", JsonValue(std::string(toString(config.background.mode))));

    JsonValue::Object display;
    display.emplace("hdrEnabled", JsonValue(config.display.hdrEnabled));

    JsonValue::Object input;
    input.emplace("leftClick", JsonValue(config.input.leftClick));
    input.emplace("middleClick", JsonValue(config.input.middleClick));
    input.emplace("rightClick", JsonValue(config.input.rightClick));
    input.emplace("samplingRateHz", JsonValue(static_cast<double>(config.input.samplingRateHz)));
    input.emplace("trailOnlyWhilePressed", JsonValue(config.input.trailOnlyWhilePressed));

    JsonValue::Object performance;
    performance.emplace("framePacing", JsonValue(std::string(toString(config.performance.framePacing))));
    performance.emplace("idleOptimization", JsonValue(config.performance.idleOptimization));

    JsonValue::Object system;
    system.emplace("closeToTray", JsonValue(config.system.closeToTray));
    system.emplace("startMinimized", JsonValue(config.system.startMinimized));
    system.emplace("startWithWindows", JsonValue(config.system.startWithWindows));

    JsonValue::Object root;
    root.emplace("background", JsonValue(std::move(background)));
    root.emplace("display", JsonValue(std::move(display)));
    root.emplace("effects", JsonValue(std::move(effects)));
    root.emplace("input", JsonValue(std::move(input)));
    root.emplace("performance", JsonValue(std::move(performance)));
    root.emplace("schemaVersion", JsonValue(static_cast<double>(currentSchemaVersion)));
    root.emplace("system", JsonValue(std::move(system)));
    return JsonValue(std::move(root));
}

[[nodiscard]] JsonValue makeFxConfigJson(const Config& config)
{
    JsonValue::Object disk;
    disk.emplace(
        "lifetimeMs",
        JsonValue(static_cast<double>(config.effects.diskLifetimeMs)));
    disk.emplace(
        "radius",
        JsonValue(static_cast<double>(config.effects.diskRadius)));

    JsonValue::Object rings;
    rings.emplace(
        "angularVelocityMultiplier",
        JsonValue(static_cast<double>(config.effects.ringsAngularVelocityMultiplier)));
    rings.emplace(
        "count",
        JsonValue(static_cast<double>(config.effects.ringsCount)));
    rings.emplace(
        "hdrIntensity",
        JsonValue(static_cast<double>(config.effects.ringsHdrIntensity)));
    rings.emplace(
        "lifetimeMs",
        JsonValue(static_cast<double>(config.effects.ringsLifetimeMs)));
    rings.emplace(
        "radiusMax",
        JsonValue(static_cast<double>(config.effects.ringsRadiusMax)));
    rings.emplace(
        "radiusMin",
        JsonValue(static_cast<double>(config.effects.ringsRadiusMin)));
    rings.emplace(
        "rotationDirection",
        JsonValue(static_cast<double>(config.effects.ringsRotationDirection)));

    JsonValue::Object shards;
    shards.emplace(
        "hdrIntensity",
        JsonValue(static_cast<double>(config.effects.shardsHdrIntensity)));

    JsonValue::Object trail;
    trail.emplace(
        "lifetimeMs",
        JsonValue(static_cast<double>(config.effects.trailLifetimeMs)));
    trail.emplace(
        "width",
        JsonValue(static_cast<double>(
            config.effects.trailWidth * referenceTrailWidthPixels)));
    trail.emplace(
        "trailOpacity",
        JsonValue(static_cast<double>(config.effects.trailOpacity)));

    JsonValue::Object bloom;
    bloom.emplace(
        "clamp",
        JsonValue(static_cast<double>(config.effects.bloomClamp)));
    bloom.emplace(
        "diffusion",
        JsonValue(static_cast<double>(config.effects.bloomDiffusion)));
    bloom.emplace(
        "intensity",
        JsonValue(static_cast<double>(config.effects.bloomIntensity)));
    bloom.emplace(
        "softKnee",
        JsonValue(static_cast<double>(config.effects.bloomSoftKnee)));
    bloom.emplace(
        "threshold",
        JsonValue(static_cast<double>(config.effects.bloomThreshold)));

    JsonValue::Object root;
    root.emplace("clickEnabled", JsonValue(config.effects.clickEnabled));
    root.emplace(
        "clickTimeScale",
        JsonValue(static_cast<double>(config.effects.clickTimeScale)));
    root.emplace("opacity", JsonValue(static_cast<double>(config.effects.opacity)));
    root.emplace("scale", JsonValue(static_cast<double>(config.effects.globalScale)));
    root.emplace("disk", JsonValue(std::move(disk)));
    root.emplace("rings", JsonValue(std::move(rings)));
    root.emplace("shards", JsonValue(std::move(shards)));
    root.emplace("trailAlways", JsonValue(!config.input.trailOnlyWhilePressed));
    root.emplace("trailEnabled", JsonValue(config.effects.trailEnabled));
    root.emplace(
        "trailTimeScale",
        JsonValue(static_cast<double>(config.effects.trailTimeScale)));
    root.emplace(
        "inputSamplingRate",
        JsonValue(static_cast<double>(config.input.samplingRateHz)));
    root.emplace("trail", JsonValue(std::move(trail)));
    root.emplace("bloom", JsonValue(std::move(bloom)));
    return JsonValue(std::move(root));
}

void appendEscapedString(std::string& output, const std::string_view value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    output.push_back('"');
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (character < 0x20U)
            {
                output += "\\u00";
                output.push_back(hex[character >> 4U]);
                output.push_back(hex[character & 0x0FU]);
            }
            else
            {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

void appendIndent(std::string& output, const std::size_t depth)
{
    output.append(depth * 2U, ' ');
}

void appendJsonValue(
    const JsonValue& value,
    std::string& output,
    const bool pretty,
    const std::size_t depth)
{
    if (std::holds_alternative<std::nullptr_t>(value.storage))
    {
        output += "null";
    }
    else if (const bool* boolean = std::get_if<bool>(&value.storage); boolean != nullptr)
    {
        output += *boolean ? "true" : "false";
    }
    else if (const double* number = std::get_if<double>(&value.storage); number != nullptr)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(9) << (*number == 0.0 ? 0.0 : *number);
        output += stream.str();
    }
    else if (const std::string* string = std::get_if<std::string>(&value.storage); string != nullptr)
    {
        appendEscapedString(output, *string);
    }
    else if (const JsonValue::Object* object = objectOf(value); object != nullptr)
    {
        output.push_back('{');
        if (!object->empty())
        {
            std::size_t index = 0U;
            for (const auto& [key, child] : *object)
            {
                if (pretty)
                {
                    output.push_back('\n');
                    appendIndent(output, depth + 1U);
                }
                appendEscapedString(output, key);
                output += pretty ? ": " : ":";
                appendJsonValue(child, output, pretty, depth + 1U);
                if (++index < object->size())
                {
                    output.push_back(',');
                }
            }
            if (pretty)
            {
                output.push_back('\n');
                appendIndent(output, depth);
            }
        }
        output.push_back('}');
    }
    else if (const JsonValue::Array* array = std::get_if<JsonValue::Array>(&value.storage);
             array != nullptr)
    {
        output.push_back('[');
        for (std::size_t index = 0U; index < array->size(); ++index)
        {
            if (index != 0U)
            {
                output.push_back(',');
            }
            if (pretty)
            {
                output.push_back('\n');
                appendIndent(output, depth + 1U);
            }
            appendJsonValue((*array)[index], output, pretty, depth + 1U);
        }
        if (pretty && !array->empty())
        {
            output.push_back('\n');
            appendIndent(output, depth);
        }
        output.push_back(']');
    }
}

[[nodiscard]] bool validateCurrentSchemaVersion(
    const JsonValue::Object& root,
    std::string& error)
{
    const JsonValue* value = member(root, "schemaVersion");
    if (value == nullptr)
    {
        error = "schemaVersion is required";
        return false;
    }
    const double* parsed = std::get_if<double>(&value->storage);
    if (parsed == nullptr || !std::isfinite(*parsed)
        || *parsed != static_cast<double>(currentSchemaVersion))
    {
        error = "schemaVersion must equal "
            + std::to_string(currentSchemaVersion);
        return false;
    }
    return true;
}

[[nodiscard]] ConfigLoadResult parseValue(std::string_view json)
{
    JsonParser parser(json);
    std::optional<JsonValue> parsed = parser.parse();
    if (!parsed.has_value())
    {
        return ConfigLoadResult{
            defaultConfig(),
            ConfigStatus::ParseError,
            parser.error().empty() ? "invalid JSON" : parser.error()};
    }
    const JsonValue::Object* originalRoot = objectOf(*parsed);
    if (originalRoot == nullptr)
    {
        return ConfigLoadResult{
            defaultConfig(),
            ConfigStatus::ParseError,
            "configuration root must be an object"};
    }

    std::string error;
    if (!validateCurrentSchemaVersion(*originalRoot, error))
    {
        return ConfigLoadResult{defaultConfig(), ConfigStatus::ValidationError, std::move(error)};
    }

    Config config = parseCurrentConfig(*originalRoot, error);
    if (!error.empty())
    {
        return ConfigLoadResult{defaultConfig(), ConfigStatus::ValidationError, std::move(error)};
    }
    if (!validateConfig(config, &error))
    {
        return ConfigLoadResult{defaultConfig(), ConfigStatus::ValidationError, std::move(error)};
    }
    return ConfigLoadResult{config, ConfigStatus::Ok, {}};
}

[[nodiscard]] std::string makeTemporarySuffix()
{
    static std::atomic<std::uint64_t> counter{0U};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::uint64_t serial = counter.fetch_add(1U, std::memory_order_relaxed);
    return ".tmp." + std::to_string(now) + "." + std::to_string(serial);
}

}

Config defaultConfig() noexcept
{
    return Config{};
}

ConfigLoadResult parseJson(const std::string_view json) noexcept
{
    try
    {
        return parseValue(json);
    }
    catch (const std::exception& error)
    {
        return ConfigLoadResult{
            defaultConfig(),
            ConfigStatus::ParseError,
            std::string("configuration parsing failed: ") + error.what()};
    }
    catch (...)
    {
        return ConfigLoadResult{
            defaultConfig(),
            ConfigStatus::ParseError,
            "configuration parsing failed"};
    }
}

ConfigPatchResult applyPatchJson(
    const Config& base,
    const std::string_view json) noexcept
{
    try
    {
        JsonParser parser(json);
        const std::optional<JsonValue> parsed = parser.parse();
        if (!parsed.has_value())
        {
            return ConfigPatchResult{
                base,
                ConfigStatus::ParseError,
                parser.error().empty() ? "invalid patch JSON" : parser.error(),
                false,
                std::nullopt};
        }

        const JsonValue::Object* root = objectOf(*parsed);
        if (root == nullptr)
        {
            return ConfigPatchResult{
                base,
                ConfigStatus::ParseError,
                "patch root must be an object",
                false,
                std::nullopt};
        }
        const JsonValue* pathValue = member(*root, "path");
        if (pathValue == nullptr)
        {
            return ConfigPatchResult{
                base,
                ConfigStatus::ParseError,
                "not a patch object",
                false,
                std::nullopt};
        }
        for (const auto& entry : *root)
        {
            if (entry.first == "path"
                || entry.first == "value"
                || entry.first == "generation")
            {
                continue;
            }

            // Patch and full-document inputs are separate contracts. Reject
            // extra members so malformed documents cannot be partly applied.
            return ConfigPatchResult{
                base,
                ConfigStatus::ValidationError,
                "patch field '" + entry.first + "' is not supported",
                true,
                std::nullopt};
        }
        const std::string* path = std::get_if<std::string>(&pathValue->storage);
        if (path == nullptr || path->empty())
        {
            return ConfigPatchResult{
                base,
                ConfigStatus::ValidationError,
                "patch path must be a non-empty string",
                true,
                std::nullopt};
        }
        const JsonValue* value = member(*root, "value");
        if (value == nullptr)
        {
            return ConfigPatchResult{
                base,
                ConfigStatus::ValidationError,
                "patch value is required",
                true,
                std::nullopt};
        }

        std::optional<std::uint64_t> expectedGeneration;
        if (const JsonValue* generation = member(*root, "generation");
            generation != nullptr)
        {
            const double* number = std::get_if<double>(&generation->storage);
            if (number == nullptr
                || !std::isfinite(*number)
                || *number < 0.0
                || std::floor(*number) != *number
                || *number
                    > static_cast<double>((std::numeric_limits<std::uint64_t>::max)()))
            {
                return ConfigPatchResult{
                    base,
                    ConfigStatus::ValidationError,
                    "patch generation must be a non-negative integer",
                    true,
                    std::nullopt};
            }
            expectedGeneration = static_cast<std::uint64_t>(*number);
        }

        Config result = base;
        const auto readPatchBool = [value](bool& target) -> bool
        {
            const bool* boolean = std::get_if<bool>(&value->storage);
            if (boolean == nullptr)
            {
                return false;
            }
            target = *boolean;
            return true;
        };
        const auto readPatchFloat = [value](float& target) -> bool
        {
            const double* number = std::get_if<double>(&value->storage);
            if (number == nullptr || !std::isfinite(*number)
                || *number < -(std::numeric_limits<float>::max)()
                || *number > (std::numeric_limits<float>::max)())
            {
                return false;
            }
            target = static_cast<float>(*number);
            return true;
        };
        const auto readPatchUnsignedInteger = [value](std::uint32_t& target) -> bool
        {
            const double* number = std::get_if<double>(&value->storage);
            if (number == nullptr || !std::isfinite(*number)
                || *number < 0.0
                || *number > static_cast<double>((std::numeric_limits<std::uint32_t>::max)())
                || std::floor(*number) != *number)
            {
                return false;
            }
            target = static_cast<std::uint32_t>(*number);
            return true;
        };
        const auto readPatchString = [value](std::string& target) -> bool
        {
            const std::string* string = std::get_if<std::string>(&value->storage);
            if (string == nullptr)
            {
                return false;
            }
            target = *string;
            return true;
        };

        bool valueAccepted = false;
        // Accept the public Web API spellings in addition to the persisted
        // native namespace. Keeping the aliases here makes IPC clients and
        // future Control Center pages share one validation contract.
        if (*path == "effects.enabled")
        {
            valueAccepted = readPatchBool(result.effects.enabled);
        }
        else if (*path == "effects.globalScale" || *path == "scale")
        {
            valueAccepted = readPatchFloat(result.effects.globalScale);
        }
        else if (*path == "effects.opacity" || *path == "opacity")
        {
            valueAccepted = readPatchFloat(result.effects.opacity);
        }
        else if (*path == "effects.clickEnabled")
        {
            valueAccepted = readPatchBool(result.effects.clickEnabled);
        }
        else if (*path == "clickEnabled")
        {
            valueAccepted = readPatchBool(result.effects.clickEnabled);
        }
        else if (*path == "effects.trailEnabled")
        {
            valueAccepted = readPatchBool(result.effects.trailEnabled);
        }
        else if (*path == "trailEnabled")
        {
            valueAccepted = readPatchBool(result.effects.trailEnabled);
        }
        else if (*path == "effects.trailLength")
        {
            valueAccepted = readPatchFloat(result.effects.trailLength);
            if (valueAccepted)
            {
                result.effects.trailLifetimeMs = result.effects.trailLength
                    * 300.0F;
            }
        }
        else if (*path == "trail.lifetimeMs")
        {
            valueAccepted = readPatchFloat(result.effects.trailLifetimeMs);
            if (valueAccepted)
            {
                result.effects.trailLength = result.effects.trailLifetimeMs
                    / 300.0F;
            }
        }
        else if (*path == "effects.trailWidth")
        {
            valueAccepted = readPatchFloat(result.effects.trailWidth);
        }
        else if (*path == "trail.width")
        {
            float webWidth = 0.0F;
            valueAccepted = readPatchFloat(webWidth);
            if (valueAccepted)
            {
                result.effects.trailWidth = webWidth
                    / referenceTrailWidthPixels;
            }
        }
        else if (*path == "effects.clickTimeScale"
            || *path == "clickTimeScale")
        {
            valueAccepted = readPatchFloat(result.effects.clickTimeScale);
        }
        else if (*path == "effects.trailTimeScale"
            || *path == "trailTimeScale")
        {
            valueAccepted = readPatchFloat(result.effects.trailTimeScale);
        }
        else if (*path == "effects.diskRadius" || *path == "disk.radius")
        {
            valueAccepted = readPatchFloat(result.effects.diskRadius);
        }
        else if (*path == "effects.diskLifetimeMs"
            || *path == "disk.lifetimeMs")
        {
            valueAccepted = readPatchFloat(result.effects.diskLifetimeMs);
        }
        else if (*path == "effects.ringsCount" || *path == "rings.count")
        {
            valueAccepted = readPatchUnsignedInteger(result.effects.ringsCount);
        }
        else if (*path == "effects.ringsLifetimeMs"
            || *path == "rings.lifetimeMs")
        {
            valueAccepted = readPatchFloat(result.effects.ringsLifetimeMs);
        }
        else if (*path == "effects.ringsRadiusMin"
            || *path == "rings.radiusMin")
        {
            valueAccepted = readPatchFloat(result.effects.ringsRadiusMin);
        }
        else if (*path == "effects.ringsRadiusMax"
            || *path == "rings.radiusMax")
        {
            valueAccepted = readPatchFloat(result.effects.ringsRadiusMax);
        }
        else if (*path == "effects.ringsAngularVelocityMultiplier"
            || *path == "rings.angularVelocityMultiplier")
        {
            valueAccepted = readPatchFloat(
                result.effects.ringsAngularVelocityMultiplier);
        }
        else if (*path == "effects.ringsRotationDirection"
            || *path == "rings.rotationDirection")
        {
            valueAccepted = readPatchFloat(result.effects.ringsRotationDirection);
        }
        else if (*path == "effects.ringsHdrIntensity"
            || *path == "rings.hdrIntensity")
        {
            valueAccepted = readPatchFloat(result.effects.ringsHdrIntensity);
        }
        else if (*path == "effects.shardsHdrIntensity"
            || *path == "shards.hdrIntensity")
        {
            valueAccepted = readPatchFloat(result.effects.shardsHdrIntensity);
        }
        else if (*path == "effects.trailOpacity"
            || *path == "trail.trailOpacity")
        {
            valueAccepted = readPatchFloat(result.effects.trailOpacity);
        }
        else if (*path == "trailAlways")
        {
            bool trailAlways = false;
            valueAccepted = readPatchBool(trailAlways);
            if (valueAccepted)
            {
                result.input.trailOnlyWhilePressed = !trailAlways;
            }
        }
        else if (*path == "inputSamplingRate")
        {
            valueAccepted = readPatchUnsignedInteger(result.input.samplingRateHz);
        }
        else if (*path == "effects.bloomIntensity")
        {
            valueAccepted = readPatchFloat(result.effects.bloomIntensity);
        }
        else if (*path == "bloom.intensity")
        {
            float webIntensity = 0.0F;
            valueAccepted = readPatchFloat(webIntensity);
            if (valueAccepted)
            {
                result.effects.bloomIntensity = webIntensity;
            }
        }
        else if (*path == "effects.bloomDiffusion"
            || *path == "bloom.diffusion")
        {
            valueAccepted = readPatchFloat(result.effects.bloomDiffusion);
        }
        else if (*path == "effects.bloomThreshold"
            || *path == "bloom.threshold")
        {
            valueAccepted = readPatchFloat(result.effects.bloomThreshold);
        }
        else if (*path == "effects.bloomSoftKnee"
            || *path == "bloom.softKnee")
        {
            valueAccepted = readPatchFloat(result.effects.bloomSoftKnee);
        }
        else if (*path == "effects.bloomClamp"
            || *path == "bloom.clamp")
        {
            valueAccepted = readPatchFloat(result.effects.bloomClamp);
        }
        else if (*path == "effects.bloomQuality")
        {
            std::string quality;
            BloomQuality parsedQuality = BloomQuality::High;
            valueAccepted = readPatchString(quality)
                && parseBloomQuality(quality, parsedQuality);
            if (valueAccepted)
            {
                result.effects.bloomDiffusion =
                    bloomDiffusionForQuality(parsedQuality);
            }
        }
        else if (*path == "background.mode")
        {
            std::string mode;
            valueAccepted = readPatchString(mode)
                && parseRenderMode(mode, result.background.mode);
        }
        else if (*path == "background.cursorExcluded")
        {
            valueAccepted = readPatchBool(result.background.cursorExcluded);
        }
        else if (*path == "background.allowSystemBorder")
        {
            valueAccepted = readPatchBool(result.background.allowSystemBorder);
        }
        else if (*path == "display.hdrEnabled")
        {
            valueAccepted = readPatchBool(result.display.hdrEnabled);
        }
        else if (*path == "input.leftClick")
        {
            valueAccepted = readPatchBool(result.input.leftClick);
        }
        else if (*path == "input.rightClick")
        {
            valueAccepted = readPatchBool(result.input.rightClick);
        }
        else if (*path == "input.middleClick")
        {
            valueAccepted = readPatchBool(result.input.middleClick);
        }
        else if (*path == "input.trailOnlyWhilePressed")
        {
            valueAccepted = readPatchBool(result.input.trailOnlyWhilePressed);
        }
        else if (*path == "input.samplingRateHz")
        {
            valueAccepted = readPatchUnsignedInteger(result.input.samplingRateHz);
        }
        else if (*path == "performance.idleOptimization")
        {
            valueAccepted = readPatchBool(result.performance.idleOptimization);
        }
        else if (*path == "performance.framePacing")
        {
            std::string pacing;
            valueAccepted = readPatchString(pacing)
                && parseFramePacing(pacing, result.performance.framePacing);
        }
        else if (*path == "system.startWithWindows")
        {
            valueAccepted = readPatchBool(result.system.startWithWindows);
        }
        else if (*path == "system.startMinimized")
        {
            valueAccepted = readPatchBool(result.system.startMinimized);
        }
        else if (*path == "system.closeToTray")
        {
            valueAccepted = readPatchBool(result.system.closeToTray);
        }
        else
        {
            return ConfigPatchResult{
                base,
                ConfigStatus::ValidationError,
                "patch path is not supported",
                true,
                expectedGeneration};
        }

        if (!valueAccepted)
        {
            return ConfigPatchResult{
                base,
                ConfigStatus::ValidationError,
                "patch value has the wrong type or enum value",
                true,
                expectedGeneration};
        }

        std::string validationError;
        if (!validateConfig(result, &validationError))
        {
            return ConfigPatchResult{
                base,
                ConfigStatus::ValidationError,
                std::move(validationError),
                true,
                expectedGeneration};
        }
        return ConfigPatchResult{
            result,
            ConfigStatus::Ok,
            {},
            true,
            expectedGeneration};
    }
    catch (const std::exception& error)
    {
        return ConfigPatchResult{
            base,
            ConfigStatus::ParseError,
            std::string("configuration patch failed: ") + error.what(),
            false,
            std::nullopt};
    }
    catch (...)
    {
        return ConfigPatchResult{
            base,
            ConfigStatus::ParseError,
            "configuration patch failed",
            false,
            std::nullopt};
    }
}

ConfigBatchPatchResult applyPatchBatchJson(
    const Config& base,
    const std::string_view json) noexcept
{
    try
    {
        JsonParser parser(json);
        const std::optional<JsonValue> parsed = parser.parse();
        if (!parsed.has_value())
        {
            return ConfigBatchPatchResult{
                base,
                ConfigStatus::ParseError,
                parser.error().empty() ? "invalid parameter patch JSON" : parser.error(),
                false};
        }

        const JsonValue::Object* root = objectOf(*parsed);
        if (root == nullptr)
        {
            return ConfigBatchPatchResult{
                base,
                ConfigStatus::ParseError,
                "parameter patch root must be an object",
                false};
        }

        std::optional<std::uint64_t> expectedGeneration;
        if (const JsonValue* generation = member(*root, "generation");
            generation != nullptr)
        {
            const double* number = std::get_if<double>(&generation->storage);
            if (number == nullptr
                || !std::isfinite(*number)
                || *number < 0.0
                || std::floor(*number) != *number
                || *number
                    > static_cast<double>((std::numeric_limits<std::uint64_t>::max)()))
            {
                return ConfigBatchPatchResult{
                    base,
                    ConfigStatus::ValidationError,
                    "parameter patch generation must be a non-negative integer",
                    true};
            }
            expectedGeneration = static_cast<std::uint64_t>(*number);
        }

        const JsonValue::Object* patchObject = root;
        if (const JsonValue* nested = member(*root, "patch"); nested != nullptr)
        {
            patchObject = objectOf(*nested);
            if (patchObject == nullptr)
            {
                return ConfigBatchPatchResult{
                    base,
                    ConfigStatus::ValidationError,
                    "parameter patch field 'patch' must be an object",
                    true};
            }
        }

        Config candidate = base;
        for (const auto& entry : *patchObject)
        {
            if (entry.first == "generation" || entry.first == "patch")
            {
                continue;
            }
            JsonValue::Object single;
            single.emplace("path", JsonValue(entry.first));
            single.emplace("value", entry.second);
            std::string singleJson;
            appendJsonValue(JsonValue(std::move(single)), singleJson, false, 0U);
            const ConfigPatchResult result = applyPatchJson(candidate, singleJson);
            if (!result.succeeded())
            {
                return ConfigBatchPatchResult{
                    base,
                    result.status,
                    "parameter '" + entry.first + "' rejected: "
                        + (result.message.empty() ? "invalid value" : result.message),
                    true,
                    expectedGeneration};
            }
            candidate = result.config;
        }
        return ConfigBatchPatchResult{
            candidate,
            ConfigStatus::Ok,
            {},
            true,
            expectedGeneration};
    }
    catch (const std::exception& error)
    {
        return ConfigBatchPatchResult{
            base,
            ConfigStatus::ParseError,
            std::string("parameter patch failed: ") + error.what(),
            false};
    }
    catch (...)
    {
        return ConfigBatchPatchResult{
            base,
            ConfigStatus::ParseError,
            "parameter patch failed",
            false};
    }
}

ConfigPatchResult setFxParam(
    const Config& base,
    const std::string_view path,
    const std::string_view valueJson) noexcept
{
    try
    {
        JsonValue::Object patch;
        patch.emplace("path", JsonValue(std::string(path)));
        JsonParser valueParser(valueJson);
        const std::optional<JsonValue> value = valueParser.parse();
        if (!value.has_value())
        {
            return ConfigPatchResult{
                base,
                ConfigStatus::ParseError,
                valueParser.error().empty()
                    ? "FX parameter value is invalid"
                    : valueParser.error(),
                false,
                std::nullopt};
        }
        patch.emplace("value", *value);
        std::string request;
        appendJsonValue(JsonValue(std::move(patch)), request, false, 0U);
        return applyPatchJson(base, request);
    }
    catch (const std::exception& error)
    {
        return ConfigPatchResult{
            base,
            ConfigStatus::ParseError,
            std::string("setFxParam failed: ") + error.what(),
            false,
            std::nullopt};
    }
    catch (...)
    {
        return ConfigPatchResult{
            base,
            ConfigStatus::ParseError,
            "setFxParam failed",
            false,
            std::nullopt};
    }
}

ConfigBatchPatchResult setFxParams(
    const Config& base,
    const std::string_view patchJson) noexcept
{
    return applyPatchBatchJson(base, patchJson);
}

std::string getFxConfig(const Config& config, const bool pretty)
{
    std::string output;
    appendJsonValue(makeFxConfigJson(config), output, pretty, 0U);
    if (pretty)
    {
        output.push_back('\n');
    }
    return output;
}

Config resetFxConfig() noexcept
{
    return defaultConfig();
}

std::string toJson(const Config& config, const bool pretty)
{
    std::string output;
    appendJsonValue(makeConfigJson(config), output, pretty, 0U);
    if (pretty)
    {
        output.push_back('\n');
    }
    return output;
}

ConfigLoadResult loadConfig(const std::filesystem::path& path) noexcept
{
    try
    {
        if (path.empty())
        {
            return ConfigLoadResult{defaultConfig(), ConfigStatus::IoError, "configuration path is empty"};
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            std::error_code status;
            const bool exists = std::filesystem::exists(path, status);
            if (!exists && !status)
            {
                return ConfigLoadResult{
                    defaultConfig(),
                    ConfigStatus::CreatedDefault,
                    "configuration file does not exist; defaults are in use"};
            }
            return ConfigLoadResult{
                defaultConfig(),
                ConfigStatus::IoError,
                "unable to open configuration file"};
        }
        constexpr std::uintmax_t maximumBytes = 1U * 1024U * 1024U;
        std::error_code status;
        const std::uintmax_t size = std::filesystem::file_size(path, status);
        if (!status && size > maximumBytes)
        {
            return ConfigLoadResult{
                defaultConfig(),
                ConfigStatus::IoError,
                "configuration file exceeds the one MiB limit"};
        }
        std::string contents(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        if (stream.bad())
        {
            return ConfigLoadResult{
                defaultConfig(),
                ConfigStatus::IoError,
                "unable to read configuration file"};
        }
        return parseJson(contents);
    }
    catch (const std::exception& error)
    {
        return ConfigLoadResult{
            defaultConfig(),
            ConfigStatus::IoError,
            std::string("unable to load configuration: ") + error.what()};
    }
    catch (...)
    {
        return ConfigLoadResult{defaultConfig(), ConfigStatus::IoError, "unable to load configuration"};
    }
}

ConfigSaveResult saveConfigAtomic(
    const std::filesystem::path& path,
    const Config& config) noexcept
{
    try
    {
        if (path.empty())
        {
            return ConfigSaveResult{ConfigStatus::WriteError, "configuration path is empty"};
        }
        std::string validationError;
        if (!validateConfig(config, &validationError))
        {
            return ConfigSaveResult{ConfigStatus::ValidationError, std::move(validationError)};
        }

        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::error_code status;
            std::filesystem::create_directories(parent, status);
            if (status)
            {
                return ConfigSaveResult{
                    ConfigStatus::WriteError,
                    "unable to create configuration directory"};
            }
        }

        // Append to the native path object so wide Windows paths stay intact.
        std::filesystem::path temporary = path;
        temporary += makeTemporarySuffix();
        const std::string serialized = toJson(config, true);
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                return ConfigSaveResult{ConfigStatus::WriteError, "unable to create temporary configuration file"};
            }
            stream.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
            stream.flush();
            if (!stream)
            {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return ConfigSaveResult{ConfigStatus::WriteError, "unable to write temporary configuration file"};
            }
        }

#if defined(_WIN32)
        if (!MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return ConfigSaveResult{ConfigStatus::WriteError, "unable to atomically replace configuration file"};
        }
#else
        if (std::rename(temporary.string().c_str(), path.string().c_str()) != 0)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return ConfigSaveResult{ConfigStatus::WriteError, "unable to atomically replace configuration file"};
        }
#endif
        return ConfigSaveResult{ConfigStatus::Ok, {}};
    }
    catch (const std::exception& error)
    {
        return ConfigSaveResult{
            ConfigStatus::WriteError,
            std::string("unable to save configuration: ") + error.what()};
    }
    catch (...)
    {
        return ConfigSaveResult{ConfigStatus::WriteError, "unable to save configuration"};
    }
}

bool validateConfig(const Config& config, std::string* error) noexcept
{
    const auto failValidation = [error](const std::string_view message)
    {
        if (error != nullptr)
        {
            *error = std::string(message);
        }
        return false;
    };

    if (config.schemaVersion != currentSchemaVersion)
    {
        return failValidation("config schemaVersion does not match the current schema");
    }
    switch (config.background.mode)
    {
    case RenderMode::BackgroundAware:
    case RenderMode::RecordingCompatible:
    case RenderMode::LightBackground:
        break;
    default:
        return failValidation("background.mode is not a recognized render mode");
    }
    switch (config.performance.framePacing)
    {
    case FramePacing::MatchDisplay:
    case FramePacing::Fixed60:
    case FramePacing::Fixed120:
    case FramePacing::Fixed144:
        break;
    default:
        return failValidation("performance.framePacing is not recognized");
    }
    if (!std::isfinite(config.effects.globalScale)
        || config.effects.globalScale < 0.1F
        || config.effects.globalScale > 4.0F)
    {
        return failValidation("effects.globalScale must be within [0.1, 4]");
    }
    if (!std::isfinite(config.effects.opacity)
        || config.effects.opacity < 0.0F
        || config.effects.opacity > 1.0F)
    {
        return failValidation("effects.opacity must be within [0, 1]");
    }
    if (!std::isfinite(config.effects.trailLength)
        || config.effects.trailLength < 0.0F
        || config.effects.trailLength > 3.0F)
    {
        return failValidation("effects.trailLength must be within [0, 3]");
    }
    if (!std::isfinite(config.effects.trailWidth)
        || config.effects.trailWidth < 0.1F
        || config.effects.trailWidth > 4.0F)
    {
        return failValidation("effects.trailWidth must be within [0.1, 4]");
    }
    if (!std::isfinite(config.effects.clickTimeScale)
        || config.effects.clickTimeScale < 0.01F
        || config.effects.clickTimeScale > 4.0F)
    {
        return failValidation("effects.clickTimeScale must be within [0.01, 4]");
    }
    if (!std::isfinite(config.effects.trailTimeScale)
        || config.effects.trailTimeScale < 0.01F
        || config.effects.trailTimeScale > 4.0F)
    {
        return failValidation("effects.trailTimeScale must be within [0.01, 4]");
    }
    if (!std::isfinite(config.effects.trailLifetimeMs)
        || config.effects.trailLifetimeMs < 0.0F
        || config.effects.trailLifetimeMs > 2000.0F)
    {
        return failValidation("effects.trailLifetimeMs must be within [0, 2000]");
    }
    if (!std::isfinite(config.effects.diskRadius)
        || config.effects.diskRadius < 1.0F
        || config.effects.diskRadius > 2000.0F)
    {
        return failValidation("effects.diskRadius must be within [1, 2000]");
    }
    if (!std::isfinite(config.effects.diskLifetimeMs)
        || config.effects.diskLifetimeMs < 1.0F
        || config.effects.diskLifetimeMs > 10000.0F)
    {
        return failValidation("effects.diskLifetimeMs must be within [1, 10000]");
    }
    if (config.effects.ringsCount > 64U)
    {
        return failValidation("effects.ringsCount must be within [0, 64]");
    }
    if (!std::isfinite(config.effects.ringsLifetimeMs)
        || config.effects.ringsLifetimeMs < 1.0F
        || config.effects.ringsLifetimeMs > 10000.0F)
    {
        return failValidation("effects.ringsLifetimeMs must be within [1, 10000]");
    }
    if (!std::isfinite(config.effects.ringsRadiusMin)
        || config.effects.ringsRadiusMin < 0.0F
        || config.effects.ringsRadiusMin > 2000.0F)
    {
        return failValidation("effects.ringsRadiusMin must be within [0, 2000]");
    }
    if (!std::isfinite(config.effects.ringsRadiusMax)
        || config.effects.ringsRadiusMax < 0.0F
        || config.effects.ringsRadiusMax > 2000.0F)
    {
        return failValidation("effects.ringsRadiusMax must be within [0, 2000]");
    }
    if (!std::isfinite(config.effects.ringsAngularVelocityMultiplier)
        || config.effects.ringsAngularVelocityMultiplier < 0.0F
        || config.effects.ringsAngularVelocityMultiplier > 100.0F)
    {
        return failValidation(
            "effects.ringsAngularVelocityMultiplier must be within [0, 100]");
    }
    if (!std::isfinite(config.effects.ringsRotationDirection)
        || config.effects.ringsRotationDirection < -1.0F
        || config.effects.ringsRotationDirection > 1.0F)
    {
        return failValidation(
            "effects.ringsRotationDirection must be within [-1, 1]");
    }
    if (!std::isfinite(config.effects.ringsHdrIntensity)
        || config.effects.ringsHdrIntensity < 0.0F
        || config.effects.ringsHdrIntensity > 64.0F)
    {
        return failValidation("effects.ringsHdrIntensity must be within [0, 64]");
    }
    if (!std::isfinite(config.effects.shardsHdrIntensity)
        || config.effects.shardsHdrIntensity < 0.0F
        || config.effects.shardsHdrIntensity > 64.0F)
    {
        return failValidation("effects.shardsHdrIntensity must be within [0, 64]");
    }
    if (!std::isfinite(config.effects.trailOpacity)
        || config.effects.trailOpacity < 0.0F
        || config.effects.trailOpacity > 1.0F)
    {
        return failValidation("effects.trailOpacity must be within [0, 1]");
    }
    if (!std::isfinite(config.effects.bloomIntensity)
        || config.effects.bloomIntensity < 0.0F
        || config.effects.bloomIntensity > 10.0F)
    {
        return failValidation("effects.bloomIntensity must be within [0, 10]");
    }
    if (!std::isfinite(config.effects.bloomDiffusion)
        || config.effects.bloomDiffusion < 0.0F
        || config.effects.bloomDiffusion > 10.0F)
    {
        return failValidation("effects.bloomDiffusion must be within [0, 10]");
    }
    if (!std::isfinite(config.effects.bloomThreshold)
        || config.effects.bloomThreshold < 0.0F
        || config.effects.bloomThreshold > 64.0F)
    {
        return failValidation("effects.bloomThreshold must be within [0, 64]");
    }
    if (!std::isfinite(config.effects.bloomSoftKnee)
        || config.effects.bloomSoftKnee < 0.0F
        || config.effects.bloomSoftKnee > 1.0F)
    {
        return failValidation("effects.bloomSoftKnee must be within [0, 1]");
    }
    if (!std::isfinite(config.effects.bloomClamp)
        || config.effects.bloomClamp < 0.0F
        || config.effects.bloomClamp > 65504.0F)
    {
        return failValidation("effects.bloomClamp must be within [0, 65504]");
    }
    if (config.input.samplingRateHz > 1000U)
    {
        return failValidation("input.samplingRateHz must be 0 or within [1, 1000]");
    }
    return true;
}

std::string_view toString(const RenderMode mode) noexcept
{
    switch (mode)
    {
    case RenderMode::BackgroundAware:
        return "background-aware";
    case RenderMode::RecordingCompatible:
        return "recording-compatible";
    case RenderMode::LightBackground:
        return "light-background";
    }
    return "background-aware";
}

std::string_view toString(const BloomQuality quality) noexcept
{
    switch (quality)
    {
    case BloomQuality::Low:
        return "low";
    case BloomQuality::Medium:
        return "medium";
    case BloomQuality::High:
        return "high";
    case BloomQuality::Ultra:
        return "ultra";
    case BloomQuality::Custom:
        return "custom";
    }
    return "high";
}

float bloomDiffusionForQuality(const BloomQuality quality) noexcept
{
    switch (quality)
    {
    case BloomQuality::Low:
        return 4.0F;
    case BloomQuality::Medium:
        return 6.0F;
    case BloomQuality::High:
        return 7.0F;
    case BloomQuality::Ultra:
        return 10.0F;
    case BloomQuality::Custom:
        break;
    }

    // A malformed enum must retain the reference visual rather than select a
    // stronger pyramid unexpectedly. Config validation rejects it upstream.
    return 7.0F;
}

BloomQuality bloomQualityForDiffusion(const float diffusion) noexcept
{
    for (const BloomQuality quality : {
             BloomQuality::Low,
             BloomQuality::Medium,
             BloomQuality::High,
             BloomQuality::Ultra})
    {
        if (diffusion == bloomDiffusionForQuality(quality))
        {
            return quality;
        }
    }
    return BloomQuality::Custom;
}

std::string_view toString(const FramePacing pacing) noexcept
{
    switch (pacing)
    {
    case FramePacing::MatchDisplay:
        return "match-display";
    case FramePacing::Fixed60:
        return "60";
    case FramePacing::Fixed120:
        return "120";
    case FramePacing::Fixed144:
        return "144";
    }
    return "match-display";
}

}

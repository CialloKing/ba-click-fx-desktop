#include "bafx/config/config.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
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

[[nodiscard]] JsonValue* member(
    JsonValue::Object& object,
    const std::string_view key) noexcept
{
    const auto iterator = object.find(key);
    return iterator == object.end() ? nullptr : &iterator->second;
}

void copyAlias(
    const JsonValue::Object& source,
    const std::string_view sourceKey,
    JsonValue::Object& destination,
    const std::string_view destinationKey)
{
    if (destination.find(destinationKey) != destination.end())
    {
        return;
    }
    if (const JsonValue* value = member(source, sourceKey); value != nullptr)
    {
        destination.emplace(std::string(destinationKey), *value);
    }
}

void moveAlias(
    JsonValue::Object& source,
    const std::string_view sourceKey,
    JsonValue::Object& destination,
    const std::string_view destinationKey)
{
    if (destination.find(destinationKey) != destination.end())
    {
        return;
    }
    const auto iterator = source.find(sourceKey);
    if (iterator == source.end())
    {
        return;
    }
    destination.emplace(std::string(destinationKey), std::move(iterator->second));
    source.erase(iterator);
}

[[nodiscard]] JsonValue::Object& ensureObject(
    JsonValue::Object& parent,
    const std::string_view key)
{
    auto iterator = parent.find(key);
    if (iterator == parent.end())
    {
        iterator = parent.emplace(std::string(key), JsonValue(JsonValue::Object{})).first;
    }
    JsonValue::Object* object = objectOf(iterator->second);
    if (object == nullptr)
    {
        iterator->second = JsonValue(JsonValue::Object{});
        object = objectOf(iterator->second);
    }
    return *object;
}

void migrateV1ToV2(JsonValue::Object& root)
{
    JsonValue::Object& effects = ensureObject(root, "effects");
    copyAlias(root, "enabled", effects, "enabled");
    copyAlias(root, "scale", effects, "globalScale");
    copyAlias(root, "trail", effects, "trailEnabled");
    copyAlias(root, "trailLength", effects, "trailLength");
    copyAlias(root, "trailWidth", effects, "trailWidth");
    copyAlias(root, "bloom", effects, "bloomIntensity");

    JsonValue::Object& background = ensureObject(root, "background");
    copyAlias(root, "backgroundMode", background, "mode");
    root["schemaVersion"] = JsonValue(2.0);
}

void migrateV2ToV3(JsonValue::Object& root)
{
    JsonValue::Object& effects = ensureObject(root, "effects");
    moveAlias(effects, "scale", effects, "globalScale");
    moveAlias(effects, "trail", effects, "trailEnabled");
    moveAlias(effects, "bloom", effects, "bloomIntensity");

    JsonValue::Object& background = ensureObject(root, "background");
    moveAlias(root, "backgroundMode", background, "mode");
    root["schemaVersion"] = JsonValue(3.0);
}

[[nodiscard]] bool readBool(
    const JsonValue::Object& object,
    const std::string_view key,
    const bool fallback,
    bool& output,
    std::string& error)
{
    const JsonValue* value = member(object, key);
    if (value == nullptr)
    {
        output = fallback;
        return true;
    }
    if (const bool* parsed = std::get_if<bool>(&value->storage); parsed != nullptr)
    {
        output = *parsed;
        return true;
    }
    error = "config field '" + std::string(key) + "' must be boolean";
    return false;
}

[[nodiscard]] bool readFloat(
    const JsonValue::Object& object,
    const std::string_view key,
    const float fallback,
    float& output,
    std::string& error)
{
    const JsonValue* value = member(object, key);
    if (value == nullptr)
    {
        output = fallback;
        return true;
    }
    const double* parsed = std::get_if<double>(&value->storage);
    if (parsed == nullptr || !std::isfinite(*parsed)
        || *parsed < -(std::numeric_limits<float>::max)()
        || *parsed > (std::numeric_limits<float>::max)())
    {
        error = "config field '" + std::string(key) + "' must be a finite number";
        return false;
    }
    output = static_cast<float>(*parsed);
    return true;
}

[[nodiscard]] bool readString(
    const JsonValue::Object& object,
    const std::string_view key,
    const std::string_view fallback,
    std::string& output,
    std::string& error)
{
    const JsonValue* value = member(object, key);
    if (value == nullptr)
    {
        output = fallback;
        return true;
    }
    const std::string* parsed = std::get_if<std::string>(&value->storage);
    if (parsed == nullptr)
    {
        error = "config field '" + std::string(key) + "' must be a string";
        return false;
    }
    output = *parsed;
    return true;
}

[[nodiscard]] bool readEnum(
    const JsonValue::Object& object,
    const std::string_view key,
    const std::string_view fallback,
    std::string& output,
    std::string& error)
{
    return readString(object, key, fallback, output, error);
}

[[nodiscard]] bool parseCaptureMode(
    const std::string_view value,
    CaptureMode& output) noexcept
{
    if (value == "fx-only" || value == "fxOnly")
    {
        output = CaptureMode::FxOnly;
        return true;
    }
    if (value == "background-aware" || value == "backgroundAware")
    {
        output = CaptureMode::BackgroundAware;
        return true;
    }
    if (value == "recording-compatible" || value == "recordingCompatible")
    {
        output = CaptureMode::RecordingCompatible;
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
    if (value == "match-display" || value == "matchDisplay")
    {
        output = FramePacing::MatchDisplay;
        return true;
    }
    if (value == "60" || value == "fixed-60")
    {
        output = FramePacing::Fixed60;
        return true;
    }
    if (value == "120" || value == "fixed-120")
    {
        output = FramePacing::Fixed120;
        return true;
    }
    if (value == "144" || value == "fixed-144")
    {
        output = FramePacing::Fixed144;
        return true;
    }
    return false;
}

[[nodiscard]] bool readNestedObject(
    const JsonValue::Object& parent,
    const std::string_view key,
    const JsonValue::Object*& output,
    std::string& error)
{
    const JsonValue* value = member(parent, key);
    if (value == nullptr)
    {
        output = nullptr;
        return true;
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

    const JsonValue::Object* effects = nullptr;
    const JsonValue::Object* background = nullptr;
    const JsonValue::Object* input = nullptr;
    const JsonValue::Object* performance = nullptr;
    const JsonValue::Object* system = nullptr;
    if (!readNestedObject(root, "effects", effects, error)
        || !readNestedObject(root, "background", background, error)
        || !readNestedObject(root, "input", input, error)
        || !readNestedObject(root, "performance", performance, error)
        || !readNestedObject(root, "system", system, error))
    {
        return config;
    }

    if (effects != nullptr)
    {
        if (!readBool(*effects, "enabled", config.effects.enabled, config.effects.enabled, error)
            || !readFloat(*effects, "globalScale", config.effects.globalScale, config.effects.globalScale, error)
            || !readBool(*effects, "clickEnabled", config.effects.clickEnabled, config.effects.clickEnabled, error)
            || !readBool(*effects, "trailEnabled", config.effects.trailEnabled, config.effects.trailEnabled, error)
            || !readFloat(*effects, "trailLength", config.effects.trailLength, config.effects.trailLength, error)
            || !readFloat(*effects, "trailWidth", config.effects.trailWidth, config.effects.trailWidth, error)
            || !readFloat(*effects, "bloomIntensity", config.effects.bloomIntensity, config.effects.bloomIntensity, error))
        {
            return config;
        }
        std::string quality;
        if (!readEnum(*effects, "bloomQuality", toString(config.effects.bloomQuality), quality, error)
            || !parseBloomQuality(quality, config.effects.bloomQuality))
        {
            error = "config field 'effects.bloomQuality' has an unknown value";
            return config;
        }
    }

    if (background != nullptr)
    {
        if (!readBool(
                *background,
                "cursorExcluded",
                config.background.cursorExcluded,
                config.background.cursorExcluded,
                error))
        {
            return config;
        }
        std::string mode;
        if (!readEnum(*background, "mode", toString(config.background.mode), mode, error)
            || !parseCaptureMode(mode, config.background.mode))
        {
            error = "config field 'background.mode' has an unknown value";
            return config;
        }
    }

    if (input != nullptr
        && (!readBool(*input, "leftClick", config.input.leftClick, config.input.leftClick, error)
            || !readBool(*input, "rightClick", config.input.rightClick, config.input.rightClick, error)
            || !readBool(*input, "middleClick", config.input.middleClick, config.input.middleClick, error)
            || !readBool(
                *input,
                "trailOnlyWhilePressed",
                config.input.trailOnlyWhilePressed,
                config.input.trailOnlyWhilePressed,
                error)))
    {
        return config;
    }

    if (performance != nullptr)
    {
        if (!readBool(
                *performance,
                "idleOptimization",
                config.performance.idleOptimization,
                config.performance.idleOptimization,
                error))
        {
            return config;
        }
        std::string pacing;
        if (!readEnum(
                *performance,
                "framePacing",
                toString(config.performance.framePacing),
                pacing,
                error)
            || !parseFramePacing(pacing, config.performance.framePacing))
        {
            error = "config field 'performance.framePacing' has an unknown value";
            return config;
        }
    }

    if (system != nullptr
        && (!readBool(
                *system,
                "startWithWindows",
                config.system.startWithWindows,
                config.system.startWithWindows,
                error)
            || !readBool(
                *system,
                "startMinimized",
                config.system.startMinimized,
                config.system.startMinimized,
                error)
            || !readBool(
                *system,
                "closeToTray",
                config.system.closeToTray,
                config.system.closeToTray,
                error)))
    {
        return config;
    }

    return config;
}

[[nodiscard]] JsonValue makeConfigJson(const Config& config)
{
    JsonValue::Object effects;
    effects.emplace("bloomIntensity", JsonValue(static_cast<double>(config.effects.bloomIntensity)));
    effects.emplace("bloomQuality", JsonValue(std::string(toString(config.effects.bloomQuality))));
    effects.emplace("clickEnabled", JsonValue(config.effects.clickEnabled));
    effects.emplace("enabled", JsonValue(config.effects.enabled));
    effects.emplace("globalScale", JsonValue(static_cast<double>(config.effects.globalScale)));
    effects.emplace("trailEnabled", JsonValue(config.effects.trailEnabled));
    effects.emplace("trailLength", JsonValue(static_cast<double>(config.effects.trailLength)));
    effects.emplace("trailWidth", JsonValue(static_cast<double>(config.effects.trailWidth)));

    JsonValue::Object background;
    background.emplace("cursorExcluded", JsonValue(config.background.cursorExcluded));
    background.emplace("mode", JsonValue(std::string(toString(config.background.mode))));

    JsonValue::Object input;
    input.emplace("leftClick", JsonValue(config.input.leftClick));
    input.emplace("middleClick", JsonValue(config.input.middleClick));
    input.emplace("rightClick", JsonValue(config.input.rightClick));
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
    root.emplace("effects", JsonValue(std::move(effects)));
    root.emplace("input", JsonValue(std::move(input)));
    root.emplace("performance", JsonValue(std::move(performance)));
    root.emplace("schemaVersion", JsonValue(static_cast<double>(currentSchemaVersion)));
    root.emplace("system", JsonValue(std::move(system)));
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

[[nodiscard]] bool readSchemaVersion(
    const JsonValue::Object& root,
    std::uint32_t& version,
    bool& wasMissing,
    std::string& error)
{
    const JsonValue* value = member(root, "schemaVersion");
    if (value == nullptr)
    {
        version = 1U;
        wasMissing = true;
        return true;
    }
    const double* parsed = std::get_if<double>(&value->storage);
    if (parsed == nullptr || !std::isfinite(*parsed)
        || *parsed < 1.0
        || *parsed > static_cast<double>((std::numeric_limits<std::uint32_t>::max)())
        || std::floor(*parsed) != *parsed)
    {
        error = "schemaVersion must be a positive integer";
        return false;
    }
    version = static_cast<std::uint32_t>(*parsed);
    wasMissing = false;
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

    JsonValue::Object root = *originalRoot;
    std::uint32_t version = 0U;
    bool missingVersion = false;
    std::string error;
    if (!readSchemaVersion(root, version, missingVersion, error))
    {
        return ConfigLoadResult{defaultConfig(), ConfigStatus::ValidationError, std::move(error)};
    }
    if (version > currentSchemaVersion)
    {
        return ConfigLoadResult{
            defaultConfig(),
            ConfigStatus::UnsupportedSchema,
            "configuration schemaVersion is newer than this build"};
    }

    const bool needsMigration = missingVersion || version < currentSchemaVersion;
    while (version < currentSchemaVersion)
    {
        if (version == 1U)
        {
            migrateV1ToV2(root);
        }
        else if (version == 2U)
        {
            migrateV2ToV3(root);
        }
        ++version;
    }

    Config config = parseCurrentConfig(root, error);
    if (!error.empty())
    {
        return ConfigLoadResult{defaultConfig(), ConfigStatus::ValidationError, std::move(error)};
    }
    if (!validateConfig(config, &error))
    {
        return ConfigLoadResult{defaultConfig(), ConfigStatus::ValidationError, std::move(error)};
    }
    return ConfigLoadResult{
        config,
        needsMigration ? ConfigStatus::Migrated : ConfigStatus::Ok,
        needsMigration ? "configuration migrated to current schema" : std::string{}};
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

        const std::filesystem::path temporary = path.string() + makeTemporarySuffix();
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
    if (!std::isfinite(config.effects.globalScale)
        || config.effects.globalScale < 0.1F
        || config.effects.globalScale > 4.0F)
    {
        return failValidation("effects.globalScale must be within [0.1, 4]");
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
    if (!std::isfinite(config.effects.bloomIntensity)
        || config.effects.bloomIntensity < 0.0F
        || config.effects.bloomIntensity > 8.0F)
    {
        return failValidation("effects.bloomIntensity must be within [0, 8]");
    }
    return true;
}

std::string_view toString(const CaptureMode mode) noexcept
{
    switch (mode)
    {
    case CaptureMode::FxOnly:
        return "fx-only";
    case CaptureMode::BackgroundAware:
        return "background-aware";
    case CaptureMode::RecordingCompatible:
        return "recording-compatible";
    }
    return "fx-only";
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
    }
    return "high";
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

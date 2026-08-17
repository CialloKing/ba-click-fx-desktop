#include "config_commands.hpp"

#include "bafx/config/config.hpp"

namespace bafx::control_center
{
namespace
{

[[nodiscard]] std::string jsonString(const std::string_view value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (character < 0x20U)
            {
                result += "\\u00";
                result.push_back(hex[character >> 4U]);
                result.push_back(hex[character & 0x0FU]);
            }
            else
            {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] std::string jsonBool(const bool value)
{
    return value ? "true" : "false";
}

}

std::string defaultConfigRequest()
{
    return "SetConfig " + bafx::config::toJson(
        bafx::config::defaultConfig(),
        false);
}

std::string setDisplayOverrideRequest(
    const std::uint64_t generation,
    const bafx::config::DisplayOverrideConfig& overrideConfig)
{
    return "SetDisplayOverride {\"generation\":"
        + std::to_string(generation)
        + ",\"displayKey\":" + jsonString(overrideConfig.displayKey)
        + ",\"enabled\":" + jsonBool(overrideConfig.enabled)
        + ",\"hdrEnabled\":" + jsonBool(overrideConfig.hdrEnabled)
        + ",\"framePacing\":"
        + jsonString(bafx::config::toString(overrideConfig.framePacing))
        + "}";
}

std::string removeDisplayOverrideRequest(
    const std::uint64_t generation,
    const std::string_view displayKey)
{
    return "RemoveDisplayOverride {\"generation\":"
        + std::to_string(generation)
        + ",\"displayKey\":" + jsonString(displayKey) + "}";
}

}

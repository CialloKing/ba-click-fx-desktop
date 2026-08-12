#include "config_commands.hpp"

#include "bafx/config/config.hpp"

namespace bafx::control_center
{

std::string defaultConfigRequest()
{
    return "SetConfig " + bafx::config::toJson(
        bafx::config::defaultConfig(),
        false);
}

}

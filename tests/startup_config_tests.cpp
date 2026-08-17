#include "test_support.hpp"

#include "startup_config.hpp"

#include "bafx/config/config.hpp"

#include <objbase.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{

class TemporaryControlCenterRoot final
{
public:
    TemporaryControlCenterRoot()
    {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid)))
        {
            throw std::runtime_error("Could not create a test directory identifier.");
        }
        wchar_t guidText[39]{};
        if (StringFromGUID2(guid, guidText, 39) == 0)
        {
            throw std::runtime_error("Could not format a test directory identifier.");
        }
        path_ = std::filesystem::temp_directory_path()
            / (L"bafx-control-center-config-" + std::wstring(guidText));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryControlCenterRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryControlCenterRoot(const TemporaryControlCenterRoot&) = delete;
    TemporaryControlCenterRoot& operator=(const TemporaryControlCenterRoot&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    void markInstalled() const
    {
        std::filesystem::create_directories(path_ / L"Installer");
        std::ofstream state(
            path_ / L"Installer" / L"INSTALL-STATE.json",
            std::ios::binary);
        state << "{}";
        if (!state)
        {
            throw std::runtime_error("Could not write the install marker.");
        }
    }

    void writeConfig(
        const std::filesystem::path& relativePath,
        const bafx::config::Config& config) const
    {
        std::filesystem::create_directories(
            (path_ / relativePath).parent_path());
        const bafx::config::ConfigSaveResult saved =
            bafx::config::saveConfigAtomic(path_ / relativePath, config);
        if (!saved.succeeded())
        {
            throw std::runtime_error("Could not write the startup config.");
        }
    }

private:
    std::filesystem::path path_{};
};

}

BAFX_TEST(control_center_startup_config_uses_portable_root)
{
    TemporaryControlCenterRoot root;
    bafx::config::Config config = bafx::config::defaultConfig();
    config.system.closeToTray = true;
    root.writeConfig(L"BAFX.config.json", config);

    BAFX_CHECK(
        bafx::control_center::startupConfigPath(root.path())
        == root.path() / L"BAFX.config.json");
    BAFX_CHECK(
        bafx::control_center::loadStartupConfig(root.path())
            .system.closeToTray);
}

BAFX_TEST(control_center_startup_config_uses_installed_data_directory)
{
    TemporaryControlCenterRoot root;
    root.markInstalled();
    bafx::config::Config rootConfig = bafx::config::defaultConfig();
    rootConfig.system.closeToTray = true;
    root.writeConfig(L"BAFX.config.json", rootConfig);
    bafx::config::Config installedConfig = bafx::config::defaultConfig();
    installedConfig.system.closeToTray = false;
    root.writeConfig(
        std::filesystem::path(L"data") / L"BAFX.config.json",
        installedConfig);

    BAFX_CHECK(
        bafx::control_center::startupConfigPath(root.path())
        == root.path() / L"data" / L"BAFX.config.json");
    BAFX_CHECK(
        !bafx::control_center::loadStartupConfig(root.path())
            .system.closeToTray);
}

BAFX_TEST(control_center_startup_config_fails_visible)
{
    TemporaryControlCenterRoot root;
    std::ofstream invalid(root.path() / L"BAFX.config.json", std::ios::binary);
    invalid << "{broken";
    invalid.close();

    BAFX_CHECK(
        !bafx::control_center::loadStartupConfig(root.path())
            .system.closeToTray);
}

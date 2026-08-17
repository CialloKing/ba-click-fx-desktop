#include "test_support.hpp"

#include "bafx/windows/startup_registration.hpp"

#include <filesystem>

using bafx::windows::controlCenterStartupCommandLine;
using bafx::windows::StartupRegistrationOperation;

BAFX_TEST(startup_registration_quotes_the_control_center_path)
{
    const std::filesystem::path path =
        LR"(C:\Program Files\ba-click-fx-desktop\BAFX.ControlCenter.exe)";
    BAFX_CHECK(
        controlCenterStartupCommandLine(path, false)
        == LR"("C:\Program Files\ba-click-fx-desktop\BAFX.ControlCenter.exe" --startup)");
    BAFX_CHECK(
        controlCenterStartupCommandLine(path, true)
        == LR"("C:\Program Files\ba-click-fx-desktop\BAFX.ControlCenter.exe" --startup --minimized)");
}

BAFX_TEST(startup_registration_rejects_an_unquotable_path)
{
    BAFX_CHECK(controlCenterStartupCommandLine({}, false).empty());
    BAFX_CHECK(
        controlCenterStartupCommandLine(
            std::filesystem::path(LR"(C:\bad"path\BAFX.ControlCenter.exe)"),
            false).empty());
}

BAFX_TEST(startup_registration_reports_stable_operation_names)
{
    using bafx::windows::startupRegistrationOperationName;

    BAFX_CHECK(
        startupRegistrationOperationName(
            StartupRegistrationOperation::ValidateCommand)
        == "validate-command");
    BAFX_CHECK(
        startupRegistrationOperationName(
            StartupRegistrationOperation::ValidateTarget)
        == "validate-target");
    BAFX_CHECK(
        startupRegistrationOperationName(StartupRegistrationOperation::OpenKey)
        == "open-key");
    BAFX_CHECK(
        startupRegistrationOperationName(
            StartupRegistrationOperation::QueryValue)
        == "query-value");
    BAFX_CHECK(
        startupRegistrationOperationName(
            StartupRegistrationOperation::CreateKey)
        == "create-key");
    BAFX_CHECK(
        startupRegistrationOperationName(StartupRegistrationOperation::SetValue)
        == "set-value");
    BAFX_CHECK(
        startupRegistrationOperationName(
            StartupRegistrationOperation::DeleteValue)
        == "delete-value");
}

BAFX_TEST(startup_registration_reports_command_validation_failure)
{
    const bafx::windows::StartupRegistrationResult result =
        bafx::windows::applyControlCenterStartupRegistration({}, true, false);

    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(
        result.status == bafx::windows::StartupRegistrationStatus::Failed);
    BAFX_CHECK(
        result.operation == StartupRegistrationOperation::ValidateCommand);
    BAFX_CHECK(result.error == ERROR_INVALID_NAME);
}

BAFX_TEST(startup_registration_reports_target_validation_failure)
{
    const bafx::windows::StartupRegistrationResult result =
        bafx::windows::applyControlCenterStartupRegistration(
            std::filesystem::temp_directory_path(),
            true,
            false);

    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(
        result.operation == StartupRegistrationOperation::ValidateTarget);
    BAFX_CHECK(result.error == ERROR_FILE_NOT_FOUND);
}

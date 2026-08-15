#include "display_output_retarget.hpp"

#include <exception>
#include <string>
#include <string_view>

namespace bafx::desktop
{
namespace
{

struct ResolvedDisplayOutputContract final
{
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    DXGI_COLOR_SPACE_TYPE applicationColorSpace{DXGI_COLOR_SPACE_CUSTOM};
    DXGI_COLOR_SPACE_TYPE displayColorSpace{DXGI_COLOR_SPACE_CUSTOM};
    std::uint32_t bitsPerColor{0U};
    bafx::windows::DisplayColorMode activeColorMode{
        bafx::windows::DisplayColorMode::Unknown};
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding{
        DISPLAYCONFIG_COLOR_ENCODING_FORCE_UINT32};
    bool advancedColorActive{false};

    [[nodiscard]] bool operator==(
        const ResolvedDisplayOutputContract&) const noexcept = default;
};

[[nodiscard]] std::optional<ResolvedDisplayOutputContract>
resolveDisplayOutputContract(
    const bafx::windows::CompositionOutputPreference preference,
    const std::optional<bafx::windows::DisplayColorCapabilities>& capabilities)
    noexcept
{
    if (preference
        == bafx::windows::CompositionOutputPreference::ConservativeSdr)
    {
        return ResolvedDisplayOutputContract{
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
            8U,
            bafx::windows::DisplayColorMode::Sdr,
            DISPLAYCONFIG_COLOR_ENCODING_RGB,
            false};
    }
    if (preference
            != bafx::windows::CompositionOutputPreference::PreferLinearScRgb
        || !capabilities.has_value())
    {
        return std::nullopt;
    }

    // scRGB keeps a fixed application-side FP16 contract. Monitor-side color
    // facts remain part of the key because DWM can remap the same transport.
    return ResolvedDisplayOutputContract{
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
        capabilities->colorSpace,
        capabilities->bitsPerColor,
        capabilities->activeColorMode,
        capabilities->colorEncoding,
        capabilities->advancedColorActive};
}

[[nodiscard]] std::string describeException(
    const std::exception_ptr& failure)
{
    try
    {
        std::rethrow_exception(failure);
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    catch (...)
    {
        return "unknown exception";
    }
}

void appendRollbackFailure(
    std::string& failures,
    const std::string_view operation,
    const std::exception_ptr& failure)
{
    if (!failures.empty())
    {
        failures += "; ";
    }
    failures += operation;
    failures += ": ";
    failures += describeException(failure);
}

}

bafx::windows::CompositionOutputPreference resolveDisplayOutputPreference(
    const bafx::windows::CompositionOutputPreference requested,
    const std::optional<bafx::windows::DisplayColorCapabilities>& capabilities)
    noexcept
{
    using bafx::windows::CompositionOutputPreference;
    using bafx::windows::DisplayColorMode;

    if (requested == CompositionOutputPreference::ConservativeSdr
        || !capabilities.has_value())
    {
        return CompositionOutputPreference::ConservativeSdr;
    }

    const bafx::windows::DisplayColorCapabilities& color = *capabilities;
    if (color.advancedColorLimitedByPolicy
        || (color.displayPathResolved
            && (!color.advancedColorStateConsistent
                || !color.advancedColorActive)))
    {
        return CompositionOutputPreference::ConservativeSdr;
    }

    if (color.activeColorMode == DisplayColorMode::Hdr
        || color.activeColorMode == DisplayColorMode::WideColorGamut)
    {
        return CompositionOutputPreference::PreferLinearScRgb;
    }
    return CompositionOutputPreference::ConservativeSdr;
}

bool displayOutputContractChanged(
    const bafx::windows::CompositionOutputPreference previousPreference,
    const bafx::windows::CompositionOutputPreference currentPreference,
    const std::optional<bafx::windows::DisplayColorCapabilities>& previous,
    const std::optional<bafx::windows::DisplayColorCapabilities>& current)
    noexcept
{
    const std::optional<ResolvedDisplayOutputContract> currentContract =
        resolveDisplayOutputContract(currentPreference, current);
    if (!currentContract.has_value())
    {
        // The policy resolver normally maps unknown capabilities to SDR. Keep
        // malformed callers from inventing a transport contract here.
        return false;
    }
    const std::optional<ResolvedDisplayOutputContract> previousContract =
        resolveDisplayOutputContract(previousPreference, previous);
    return !previousContract.has_value()
        || *previousContract != *currentContract;
}

DisplayOutputRetargetResult retargetDisplayOutput(
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    const DisplayOutputRetargetIntent& intent)
{
    const std::optional<RECT> previousBounds = intent.windowBounds.has_value()
        ? std::optional<RECT>(window.bounds())
        : std::nullopt;
    const std::optional<LUID> previousAdapter =
        renderer.deviceInfo().requestedAdapterLuid;
    const bafx::windows::WindowSize previousOutputSize = renderer.outputSize();
    const bafx::windows::CompositionOutputPreference previousPreference =
        renderer.outputPreference();
    const bafx::windows::CompositionOutputPreference targetPreference =
        intent.outputPreference.value_or(previousPreference);

    DisplayOutputRetargetResult result{};
    try
    {
        if (intent.windowBounds.has_value())
        {
            // Geometry is validated before replacing the more expensive D3D
            // resource domain, but it remains part of the same transaction.
            window.setBounds(*intent.windowBounds);
            if (window.size().width != intent.outputSize.width
                || window.size().height != intent.outputSize.height)
            {
                throw std::runtime_error(
                    "Overlay client size does not match the display output");
            }
        }
        result.adapter = renderer.retargetOutputAdapter(
            intent.requestedAdapterLuid);
        result.deviceBeforeResize = renderer.deviceInfo();
        result.output = renderer.resizeOutput(intent.outputSize);
        const bool outputResourceDomainRecreated =
            result.adapter
                != bafx::windows::OutputAdapterRetargetStatus::Unchanged
            || result.output
                == bafx::windows::OutputResizeStatus::DeviceRecovered;
        const bool outputPreferenceChanged =
            targetPreference != renderer.outputPreference();
        if (outputPreferenceChanged
            || (intent.windowBounds.has_value()
                && !outputResourceDomainRecreated))
        {
            // A same-adapter move can keep the old pixel size, while an HDR
            // policy change can follow a resize-created SDR swap chain. In
            // both cases DXGI must evaluate the target monitor after the move.
            result.deviceBeforeOutputRenegotiation = renderer.deviceInfo();
            result.outputRenegotiation = renderer.renegotiateOutput(
                targetPreference);
        }
        return result;
    }
    catch (...)
    {
        const std::exception_ptr retargetFailure = std::current_exception();
        std::string rollbackFailures;
        const auto attemptRollback =
            [&](const std::string_view operation, auto&& rollback)
        {
            try
            {
                rollback();
            }
            catch (...)
            {
                appendRollbackFailure(
                    rollbackFailures,
                    operation,
                    std::current_exception());
            }
        };

        // Try every component independently. A broken HWND restore must not
        // prevent recovery of the old adapter and swap-chain dimensions.
        if (previousBounds.has_value())
        {
            attemptRollback("window", [&]()
            {
                window.setBounds(*previousBounds);
            });
        }
        attemptRollback("adapter", [&]()
        {
            static_cast<void>(renderer.retargetOutputAdapter(previousAdapter));
        });
        attemptRollback("output", [&]()
        {
            const bafx::windows::WindowSize currentSize =
                renderer.outputSize();
            const bool outputSizeChanged =
                currentSize.width != previousOutputSize.width
                || currentSize.height != previousOutputSize.height;
            if (outputSizeChanged)
            {
                static_cast<void>(renderer.resizeOutput(previousOutputSize));
            }

            const bool outputPreferenceChanged =
                renderer.outputPreference() != previousPreference;
            if (previousBounds.has_value()
                || !outputSizeChanged
                || outputPreferenceChanged)
            {
                // A recovered device may have selected the moved monitor's
                // color contract before a later operation failed. Re-evaluate
                // after restoring the HWND; same-size resize failures also
                // need a replacement swap chain to restore the released RTV.
                static_cast<void>(renderer.renegotiateOutput(
                    previousPreference));
            }
        });

        if (rollbackFailures.empty())
        {
            std::rethrow_exception(retargetFailure);
        }
        throw DisplayOutputRollbackError(
            "Display output retarget failed: "
            + describeException(retargetFailure)
            + "; rollback failed: " + rollbackFailures);
    }
}

}

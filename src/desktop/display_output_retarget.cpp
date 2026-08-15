#include "display_output_retarget.hpp"

#include <exception>
#include <string>
#include <string_view>

namespace bafx::desktop
{
namespace
{

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
        if (intent.windowBounds.has_value()
            && !outputResourceDomainRecreated)
        {
            // A same-adapter monitor move can also keep the old pixel size.
            // Recreate the swap chain after moving the HWND so DXGI evaluates
            // the new monitor's Advanced Color presentation contract.
            result.deviceBeforeOutputRenegotiation = renderer.deviceInfo();
            result.outputRenegotiation = renderer.renegotiateOutput(
                renderer.outputPreference());
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

            if (previousBounds.has_value() || !outputSizeChanged)
            {
                // A recovered device may have selected the moved monitor's
                // color contract before a later operation failed. Re-evaluate
                // after restoring the HWND; same-size resize failures also
                // need a replacement swap chain to restore the released RTV.
                static_cast<void>(renderer.renegotiateOutput(
                    renderer.outputPreference()));
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

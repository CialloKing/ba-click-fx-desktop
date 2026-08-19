#pragma once

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace bafx::windows
{

enum class Spout2SenderStatus : std::uint8_t
{
    Disabled,
    WaitingForFrame,
    Sent,
    Unavailable,
    Failed
};

[[nodiscard]] constexpr std::string_view spout2SenderStatusName(
    const Spout2SenderStatus status) noexcept
{
    switch (status)
    {
    case Spout2SenderStatus::Disabled:
        return "disabled";
    case Spout2SenderStatus::WaitingForFrame:
        return "waiting-for-frame";
    case Spout2SenderStatus::Sent:
        return "sent";
    case Spout2SenderStatus::Unavailable:
        return "unavailable";
    case Spout2SenderStatus::Failed:
        return "failed";
    }
    return "failed";
}

// Owns the optional Spout2 sender but never owns the renderer's immediate
// context. Spout2 is fed only after the opaque recording texture is complete.
class Spout2Sender final
{
public:
    explicit Spout2Sender(std::string senderName = "ba-click-fx-desktop");
    ~Spout2Sender();

    Spout2Sender(const Spout2Sender&) = delete;
    Spout2Sender& operator=(const Spout2Sender&) = delete;

    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] Spout2SenderStatus status() const noexcept;
    [[nodiscard]] std::string_view senderName() const noexcept;
    [[nodiscard]] std::string_view error() const noexcept;

    // SendTexture performs the SDK's cross-process GPU handoff. The caller
    // remains responsible for ensuring texture dimensions and format are the
    // negotiated BGRA8 recording contract.
    [[nodiscard]] bool send(
        ID3D11Device* device,
        ID3D11Texture2D* texture,
        std::uint32_t width,
        std::uint32_t height) noexcept;
    void reset() noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}

#include "bafx/windows/spout2_sender.hpp"

#include <utility>

#if defined(BAFX_ENABLE_SPOUT2)
#include <SpoutDX/SpoutDX.h>
#endif

namespace bafx::windows
{

struct Spout2Sender::Implementation final
{
    explicit Implementation(std::string nextSenderName)
        : senderName(std::move(nextSenderName))
    {
    }

    std::string senderName{};
    std::string errorMessage{};
    Spout2SenderStatus state{Spout2SenderStatus::Disabled};
    bool enabled{false};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
#if defined(BAFX_ENABLE_SPOUT2)
    spoutDX sender{};
    bool directXOpened{false};
#endif
};

Spout2Sender::Spout2Sender(std::string senderName)
    : implementation_(std::make_unique<Implementation>(std::move(senderName)))
{
}

Spout2Sender::~Spout2Sender()
{
    reset();
}

void Spout2Sender::setEnabled(const bool enabled) noexcept
{
    implementation_->enabled = enabled;
    if (!enabled)
    {
        reset();
        return;
    }
    if (implementation_->state == Spout2SenderStatus::Disabled)
    {
        implementation_->state = Spout2SenderStatus::WaitingForFrame;
    }
}

bool Spout2Sender::enabled() const noexcept
{
    return implementation_->enabled;
}

Spout2SenderStatus Spout2Sender::status() const noexcept
{
    return implementation_->state;
}

std::string_view Spout2Sender::senderName() const noexcept
{
    return implementation_->senderName;
}

std::string_view Spout2Sender::error() const noexcept
{
    return implementation_->errorMessage;
}

bool Spout2Sender::send(
    ID3D11Device* const device,
    ID3D11Texture2D* const texture,
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    if (!implementation_->enabled)
    {
        implementation_->state = Spout2SenderStatus::Disabled;
        return false;
    }
    if (device == nullptr || texture == nullptr || width == 0U || height == 0U)
    {
        implementation_->state = Spout2SenderStatus::Failed;
        implementation_->errorMessage = "invalid Spout2 recording texture";
        return false;
    }

#if !defined(BAFX_ENABLE_SPOUT2)
    (void)device;
    (void)texture;
    (void)width;
    (void)height;
    implementation_->state = Spout2SenderStatus::Unavailable;
    implementation_->errorMessage = "Spout2 support was not built";
    return false;
#else
    try
    {
        if (!implementation_->directXOpened)
        {
            if (!implementation_->sender.OpenDirectX11(device))
            {
                implementation_->state = Spout2SenderStatus::Unavailable;
                implementation_->errorMessage =
                    "Spout2 could not open the Host D3D11 device";
                return false;
            }
            implementation_->sender.SetKeyed(true);
            implementation_->sender.SetSenderFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM);
            implementation_->sender.SetSenderName(
                implementation_->senderName.c_str());
            implementation_->directXOpened = true;
        }

        if (!implementation_->sender.SendTexture(texture))
        {
            implementation_->state = Spout2SenderStatus::Failed;
            implementation_->errorMessage = "Spout2 SendTexture failed";
            return false;
        }
        implementation_->width = width;
        implementation_->height = height;
        implementation_->errorMessage.clear();
        implementation_->state = Spout2SenderStatus::Sent;
        return true;
    }
    catch (...)
    {
        implementation_->state = Spout2SenderStatus::Failed;
        implementation_->errorMessage = "Spout2 sender raised an exception";
        return false;
    }
#endif
}

void Spout2Sender::reset() noexcept
{
#if defined(BAFX_ENABLE_SPOUT2)
    if (implementation_->directXOpened)
    {
        implementation_->sender.ReleaseSender();
        implementation_->sender.CloseDirectX11();
        implementation_->directXOpened = false;
    }
#endif
    implementation_->width = 0U;
    implementation_->height = 0U;
    implementation_->errorMessage.clear();
    implementation_->state = implementation_->enabled
        ? Spout2SenderStatus::WaitingForFrame
        : Spout2SenderStatus::Disabled;
}

}

#include "update_check.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <utility>

namespace bafx::release_update
{
namespace
{

constexpr std::size_t maximumJsonDepth = 64U;

[[nodiscard]] bool isDigit(const char value) noexcept
{
    return value >= '0' && value <= '9';
}

[[nodiscard]] bool parseVersionComponent(
    const std::string_view text,
    std::size_t& position,
    std::uint32_t& result) noexcept
{
    if (position >= text.size() || !isDigit(text[position]))
    {
        return false;
    }

    const std::size_t firstDigit = position;
    std::uint32_t value = 0U;
    while (position < text.size() && isDigit(text[position]))
    {
        const std::uint32_t digit =
            static_cast<std::uint32_t>(text[position] - '0');
        if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U)
        {
            return false;
        }

        value = value * 10U + digit;
        ++position;
    }

    // Canonical tags have one representation. Rejecting leading zeroes also
    // prevents visually distinct tags from comparing as the same release.
    if (position - firstDigit > 1U && text[firstDigit] == '0')
    {
        return false;
    }

    result = value;
    return true;
}

class JsonParser final
{
public:
    explicit JsonParser(const std::string_view text) noexcept
        : text_(text)
    {
    }

    [[nodiscard]] bool parse()
    {
        skipWhitespace();
        if (!parseObject(true, 0U))
        {
            return false;
        }

        skipWhitespace();
        return position_ == text_.size();
    }

    [[nodiscard]] std::size_t tagNameCount() const noexcept
    {
        return tagNameCount_;
    }

    [[nodiscard]] bool tagNameWasString() const noexcept
    {
        return tagNameWasString_;
    }

    [[nodiscard]] const std::string& tagName() const noexcept
    {
        return tagName_;
    }

private:
    void skipWhitespace() noexcept
    {
        while (position_ < text_.size())
        {
            const char value = text_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
            {
                return;
            }

            ++position_;
        }
    }

    [[nodiscard]] bool consume(const char expected) noexcept
    {
        if (position_ >= text_.size() || text_[position_] != expected)
        {
            return false;
        }

        ++position_;
        return true;
    }

    [[nodiscard]] bool consume(const std::string_view expected) noexcept
    {
        if (expected.size() > text_.size() - position_
            || text_.substr(position_, expected.size()) != expected)
        {
            return false;
        }

        position_ += expected.size();
        return true;
    }

    [[nodiscard]] bool parseObject(
        const bool topLevel,
        const std::size_t depth)
    {
        if (depth > maximumJsonDepth || !consume('{'))
        {
            return false;
        }

        skipWhitespace();
        if (consume('}'))
        {
            return true;
        }

        while (position_ < text_.size())
        {
            std::string key;
            if (!parseString(topLevel ? &key : nullptr))
            {
                return false;
            }

            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }

            skipWhitespace();
            if (topLevel && key == "tag_name")
            {
                ++tagNameCount_;
                if (position_ < text_.size() && text_[position_] == '"')
                {
                    std::string candidate;
                    if (!parseString(&candidate))
                    {
                        return false;
                    }

                    if (tagNameCount_ == 1U)
                    {
                        tagNameWasString_ = true;
                        tagName_ = std::move(candidate);
                    }
                }
                else
                {
                    if (!skipValue(depth + 1U))
                    {
                        return false;
                    }

                    if (tagNameCount_ == 1U)
                    {
                        tagNameWasString_ = false;
                    }
                }
            }
            else if (!skipValue(depth + 1U))
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                return true;
            }

            if (!consume(','))
            {
                return false;
            }

            skipWhitespace();
        }

        return false;
    }

    [[nodiscard]] bool parseArray(const std::size_t depth)
    {
        if (depth > maximumJsonDepth || !consume('['))
        {
            return false;
        }

        skipWhitespace();
        if (consume(']'))
        {
            return true;
        }

        while (position_ < text_.size())
        {
            if (!skipValue(depth + 1U))
            {
                return false;
            }

            skipWhitespace();
            if (consume(']'))
            {
                return true;
            }

            if (!consume(','))
            {
                return false;
            }

            skipWhitespace();
        }

        return false;
    }

    [[nodiscard]] bool skipValue(const std::size_t depth)
    {
        if (depth > maximumJsonDepth || position_ >= text_.size())
        {
            return false;
        }

        switch (text_[position_])
        {
        case '"':
            return parseString(nullptr);
        case '{':
            return parseObject(false, depth);
        case '[':
            return parseArray(depth);
        case 't':
            return consume("true");
        case 'f':
            return consume("false");
        case 'n':
            return consume("null");
        default:
            return parseNumber();
        }
    }

    [[nodiscard]] bool parseNumber() noexcept
    {
        if (consume('-') && position_ >= text_.size())
        {
            return false;
        }

        if (consume('0'))
        {
            if (position_ < text_.size() && isDigit(text_[position_]))
            {
                return false;
            }
        }
        else
        {
            if (position_ >= text_.size()
                || text_[position_] < '1'
                || text_[position_] > '9')
            {
                return false;
            }

            while (position_ < text_.size() && isDigit(text_[position_]))
            {
                ++position_;
            }
        }

        if (consume('.'))
        {
            if (position_ >= text_.size() || !isDigit(text_[position_]))
            {
                return false;
            }

            while (position_ < text_.size() && isDigit(text_[position_]))
            {
                ++position_;
            }
        }

        if (position_ < text_.size()
            && (text_[position_] == 'e' || text_[position_] == 'E'))
        {
            ++position_;
            if (position_ < text_.size()
                && (text_[position_] == '+' || text_[position_] == '-'))
            {
                ++position_;
            }

            if (position_ >= text_.size() || !isDigit(text_[position_]))
            {
                return false;
            }

            while (position_ < text_.size() && isDigit(text_[position_]))
            {
                ++position_;
            }
        }

        return true;
    }

    [[nodiscard]] bool parseString(std::string* output)
    {
        if (!consume('"'))
        {
            return false;
        }

        while (position_ < text_.size())
        {
            const unsigned char value =
                static_cast<unsigned char>(text_[position_]);
            if (value == static_cast<unsigned char>('"'))
            {
                ++position_;
                return true;
            }

            if (value < 0x20U)
            {
                return false;
            }

            if (value == static_cast<unsigned char>('\\'))
            {
                ++position_;
                if (!parseEscape(output))
                {
                    return false;
                }

                continue;
            }

            if (value < 0x80U)
            {
                if (output != nullptr)
                {
                    output->push_back(static_cast<char>(value));
                }

                ++position_;
                continue;
            }

            if (!parseUtf8(output))
            {
                return false;
            }
        }

        return false;
    }

    [[nodiscard]] bool parseEscape(std::string* output)
    {
        if (position_ >= text_.size())
        {
            return false;
        }

        const char escape = text_[position_++];
        switch (escape)
        {
        case '"':
        case '\\':
        case '/':
            append(output, escape);
            return true;
        case 'b':
            append(output, '\b');
            return true;
        case 'f':
            append(output, '\f');
            return true;
        case 'n':
            append(output, '\n');
            return true;
        case 'r':
            append(output, '\r');
            return true;
        case 't':
            append(output, '\t');
            return true;
        case 'u':
            return parseEscapedCodePoint(output);
        default:
            return false;
        }
    }

    [[nodiscard]] bool parseEscapedCodePoint(std::string* output)
    {
        std::uint32_t codePoint = 0U;
        if (!parseHexQuad(codePoint))
        {
            return false;
        }

        if (codePoint >= 0xD800U && codePoint <= 0xDBFFU)
        {
            if (position_ + 2U > text_.size()
                || text_[position_] != '\\'
                || text_[position_ + 1U] != 'u')
            {
                return false;
            }

            position_ += 2U;
            std::uint32_t lowSurrogate = 0U;
            if (!parseHexQuad(lowSurrogate)
                || lowSurrogate < 0xDC00U
                || lowSurrogate > 0xDFFFU)
            {
                return false;
            }

            codePoint = 0x10000U
                + ((codePoint - 0xD800U) << 10U)
                + (lowSurrogate - 0xDC00U);
        }
        else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU)
        {
            return false;
        }

        appendCodePoint(output, codePoint);
        return true;
    }

    [[nodiscard]] bool parseHexQuad(std::uint32_t& result) noexcept
    {
        if (position_ + 4U > text_.size())
        {
            return false;
        }

        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index)
        {
            const char digit = text_[position_++];
            value <<= 4U;
            if (digit >= '0' && digit <= '9')
            {
                value += static_cast<std::uint32_t>(digit - '0');
            }
            else if (digit >= 'a' && digit <= 'f')
            {
                value += static_cast<std::uint32_t>(digit - 'a' + 10);
            }
            else if (digit >= 'A' && digit <= 'F')
            {
                value += static_cast<std::uint32_t>(digit - 'A' + 10);
            }
            else
            {
                return false;
            }
        }

        result = value;
        return true;
    }

    [[nodiscard]] bool parseUtf8(std::string* output)
    {
        const std::size_t first = position_;
        const unsigned char lead = static_cast<unsigned char>(text_[position_]);
        std::size_t length = 0U;
        std::uint32_t codePoint = 0U;
        std::uint32_t minimum = 0U;
        if (lead >= 0xC2U && lead <= 0xDFU)
        {
            length = 2U;
            codePoint = lead & 0x1FU;
            minimum = 0x80U;
        }
        else if (lead >= 0xE0U && lead <= 0xEFU)
        {
            length = 3U;
            codePoint = lead & 0x0FU;
            minimum = 0x800U;
        }
        else if (lead >= 0xF0U && lead <= 0xF4U)
        {
            length = 4U;
            codePoint = lead & 0x07U;
            minimum = 0x10000U;
        }
        else
        {
            return false;
        }

        if (length > text_.size() - position_)
        {
            return false;
        }

        for (std::size_t index = 1U; index < length; ++index)
        {
            const unsigned char continuation =
                static_cast<unsigned char>(text_[position_ + index]);
            if ((continuation & 0xC0U) != 0x80U)
            {
                return false;
            }

            codePoint = (codePoint << 6U) | (continuation & 0x3FU);
        }

        if (codePoint < minimum
            || codePoint > 0x10FFFFU
            || (codePoint >= 0xD800U && codePoint <= 0xDFFFU))
        {
            return false;
        }

        position_ += length;
        if (output != nullptr)
        {
            output->append(text_.substr(first, length));
        }

        return true;
    }

    static void append(std::string* output, const char value)
    {
        if (output != nullptr)
        {
            output->push_back(value);
        }
    }

    static void appendCodePoint(std::string* output, const std::uint32_t codePoint)
    {
        if (output == nullptr)
        {
            return;
        }

        if (codePoint <= 0x7FU)
        {
            output->push_back(static_cast<char>(codePoint));
        }
        else if (codePoint <= 0x7FFU)
        {
            output->push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output->push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        else if (codePoint <= 0xFFFFU)
        {
            output->push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output->push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            output->push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        else
        {
            output->push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
            output->push_back(static_cast<char>(
                0x80U | ((codePoint >> 12U) & 0x3FU)));
            output->push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            output->push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }

    std::string_view text_{};
    std::size_t position_{0U};
    std::size_t tagNameCount_{0U};
    bool tagNameWasString_{false};
    std::string tagName_{};
};

[[nodiscard]] std::string responseFailure(
    const ReleaseResponseError error)
{
    switch (error)
    {
    case ReleaseResponseError::ResponseTooLarge:
        return "GitHub response exceeds 256 KiB";
    case ReleaseResponseError::MalformedJson:
        return "GitHub response is not valid JSON";
    case ReleaseResponseError::MissingTagName:
        return "GitHub response has no tag_name";
    case ReleaseResponseError::DuplicateTagName:
        return "GitHub response repeats tag_name";
    case ReleaseResponseError::InvalidTagName:
        return "GitHub tag_name is not vMAJOR.MINOR.PATCH";
    case ReleaseResponseError::None:
        break;
    }

    return "GitHub response is invalid";
}

}

std::optional<ReleaseVersion> parseReleaseVersion(
    const std::string_view text) noexcept
{
    if (text.size() < 6U || text.front() != 'v')
    {
        return std::nullopt;
    }

    ReleaseVersion version{};
    std::size_t position = 1U;
    if (!parseVersionComponent(text, position, version.major)
        || position >= text.size()
        || text[position++] != '.'
        || !parseVersionComponent(text, position, version.minor)
        || position >= text.size()
        || text[position++] != '.'
        || !parseVersionComponent(text, position, version.patch)
        || position != text.size())
    {
        return std::nullopt;
    }

    return version;
}

ReleaseResponseParseResult parseReleaseResponse(
    const std::string_view response) noexcept
{
    if (response.size() > maximumResponseBytes)
    {
        return ReleaseResponseParseResult{
            .error = ReleaseResponseError::ResponseTooLarge};
    }

    try
    {
        JsonParser parser(response);
        if (!parser.parse())
        {
            return ReleaseResponseParseResult{
                .error = ReleaseResponseError::MalformedJson};
        }

        if (parser.tagNameCount() > 1U)
        {
            return ReleaseResponseParseResult{
                .error = ReleaseResponseError::DuplicateTagName};
        }

        if (parser.tagNameCount() == 0U)
        {
            return ReleaseResponseParseResult{
                .error = ReleaseResponseError::MissingTagName};
        }

        if (!parser.tagNameWasString())
        {
            return ReleaseResponseParseResult{
                .error = ReleaseResponseError::InvalidTagName};
        }

        const std::optional<ReleaseVersion> version =
            parseReleaseVersion(parser.tagName());
        if (!version.has_value())
        {
            return ReleaseResponseParseResult{
                .tagName = parser.tagName(),
                .error = ReleaseResponseError::InvalidTagName};
        }

        return ReleaseResponseParseResult{
            .version = version,
            .tagName = parser.tagName(),
            .error = ReleaseResponseError::None};
    }
    catch (...)
    {
        return ReleaseResponseParseResult{
            .error = ReleaseResponseError::MalformedJson};
    }
}

ReleaseUpdateChecker::ReleaseUpdateChecker(
    const ReleaseVersion currentVersion,
    std::shared_ptr<ReleaseTransport> transport)
    : currentVersion_(currentVersion),
      transport_(std::move(transport))
{
}

ReleaseUpdateChecker::~ReleaseUpdateChecker()
{
    std::scoped_lock lock(lifecycleMutex_);
    stopWorker(false);
}

bool ReleaseUpdateChecker::start()
{
    std::scoped_lock lifecycleLock(lifecycleMutex_);
    {
        std::scoped_lock stateLock(stateMutex_);
        if (state_.status == UpdateCheckStatus::Checking)
        {
            return false;
        }
    }

    if (worker_.joinable())
    {
        worker_.join();
    }

    if (transport_ == nullptr)
    {
        publish(UpdateCheckSnapshot{
            .status = UpdateCheckStatus::Failed,
            .failure = "No release transport is configured"});
        return false;
    }

    publish(UpdateCheckSnapshot{.status = UpdateCheckStatus::Checking});
    try
    {
        worker_ = std::jthread(
            [this](const std::stop_token stopToken)
            {
                run(stopToken);
            });
    }
    catch (const std::exception& error)
    {
        publish(UpdateCheckSnapshot{
            .status = UpdateCheckStatus::Failed,
            .failure = std::string("Unable to start update check: ") + error.what()});
        return false;
    }
    catch (...)
    {
        publish(UpdateCheckSnapshot{
            .status = UpdateCheckStatus::Failed,
            .failure = "Unable to start update check"});
        return false;
    }

    return true;
}

void ReleaseUpdateChecker::cancel()
{
    std::scoped_lock lifecycleLock(lifecycleMutex_);
    stopWorker(true);
}

UpdateCheckSnapshot ReleaseUpdateChecker::snapshot() const
{
    std::scoped_lock lock(stateMutex_);
    return state_;
}

void ReleaseUpdateChecker::run(const std::stop_token stopToken) noexcept
{
    try
    {
        ReleaseTransportResult transportResult =
            transport_->fetchLatestRelease(stopToken);
        if (stopToken.stop_requested())
        {
            return;
        }

        if (transportResult.status == ReleaseTransportStatus::Cancelled)
        {
            publish(UpdateCheckSnapshot{.status = UpdateCheckStatus::Idle});
            return;
        }

        if (transportResult.status != ReleaseTransportStatus::Succeeded)
        {
            publish(UpdateCheckSnapshot{
                .status = UpdateCheckStatus::Failed,
                .failure = transportResult.failure.empty()
                    ? "Update request failed"
                    : std::move(transportResult.failure)});
            return;
        }

        if (transportResult.httpStatus != 200U)
        {
            publish(UpdateCheckSnapshot{
                .status = UpdateCheckStatus::Failed,
                .failure = "GitHub API returned HTTP "
                    + std::to_string(transportResult.httpStatus)});
            return;
        }

        const ReleaseResponseParseResult parsed =
            parseReleaseResponse(transportResult.body);
        if (!parsed.succeeded())
        {
            publish(UpdateCheckSnapshot{
                .status = UpdateCheckStatus::Failed,
                .failure = responseFailure(parsed.error)});
            return;
        }

        if (stopToken.stop_requested())
        {
            return;
        }

        UpdateCheckStatus status = UpdateCheckStatus::Current;
        if (*parsed.version > currentVersion_)
        {
            status = UpdateCheckStatus::UpdateAvailable;
        }
        else if (*parsed.version < currentVersion_)
        {
            status = UpdateCheckStatus::Ahead;
        }

        publish(UpdateCheckSnapshot{
            .status = status,
            .latestVersion = parsed.version,
            .latestTagName = parsed.tagName});
    }
    catch (const std::exception& error)
    {
        if (!stopToken.stop_requested())
        {
            try
            {
                publish(UpdateCheckSnapshot{
                    .status = UpdateCheckStatus::Failed,
                    .failure = std::string("Update check failed: ") + error.what()});
            }
            catch (...)
            {
            }
        }
    }
    catch (...)
    {
        if (!stopToken.stop_requested())
        {
            try
            {
                publish(UpdateCheckSnapshot{
                    .status = UpdateCheckStatus::Failed,
                    .failure = "Update check failed"});
            }
            catch (...)
            {
            }
        }
    }
}

void ReleaseUpdateChecker::publish(UpdateCheckSnapshot snapshot) noexcept
{
    std::scoped_lock lock(stateMutex_);
    snapshot.sequence = state_.sequence + 1U;
    state_ = std::move(snapshot);
}

void ReleaseUpdateChecker::stopWorker(const bool resetToIdle)
{
    bool wasChecking = false;
    {
        std::scoped_lock stateLock(stateMutex_);
        wasChecking = state_.status == UpdateCheckStatus::Checking;
    }

    if (worker_.joinable())
    {
        if (wasChecking)
        {
            worker_.request_stop();
            if (transport_ != nullptr)
            {
                transport_->cancel();
            }
        }

        worker_.join();
    }

    if (resetToIdle && wasChecking)
    {
        publish(UpdateCheckSnapshot{.status = UpdateCheckStatus::Idle});
    }
}

std::wstring_view officialLatestReleasePageUrl() noexcept
{
    return L"https://github.com/CialloKing/ba-click-fx-desktop/releases/latest";
}

std::wstring_view officialProjectRepositoryUrl() noexcept
{
    return L"https://github.com/CialloKing/ba-click-fx-desktop";
}

}

#include "Protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace
{
    bool isValidPositionMilliseconds(long long milliseconds)
    {
        if (milliseconds < 0)
        {
            return false;
        }

        // SyncState 同时保留 int 秒数，解析阶段就拒绝无法安全转换的超大位置，
        // 避免后续 static_cast<int> 产生实现相关的截断结果。
        return milliseconds / 1000 <= (std::numeric_limits<int>::max)();
    }

    bool stringToPlaybackState(const std::string& text, PlaybackState& state)
    {
        if (text == "Playing")
        {
            state = PlaybackState::Playing;
            return true;
        }

        if (text == "Paused")
        {
            state = PlaybackState::Paused;
            return true;
        }

        if (text == "Stopped")
        {
            state = PlaybackState::Stopped;
            return true;
        }

        return false;
    }

    bool isValidMediaIdentity(const std::string& identity)
    {
        return identity.size() == 16 &&
            std::all_of(
                identity.begin(),
                identity.end(),
                [](unsigned char character)
                {
                    return std::isxdigit(character) != 0;
                }
            );
    }

    bool isValidRejectionReason(const std::string& reason)
    {
        if (reason.empty() || reason.size() > 64)
        {
            return false;
        }

        return std::all_of(
            reason.begin(),
            reason.end(),
            [](unsigned char character)
            {
                return std::isupper(character) != 0 ||
                    std::isdigit(character) != 0 ||
                    character == '_';
            }
        );
    }

    bool stringToCorrectionResultStatus(
        const std::string& text,
        CorrectionResultStatus& status)
    {
        if (text == "APPLIED")
        {
            status = CorrectionResultStatus::Applied;
            return true;
        }
        if (text == "REJECTED")
        {
            status = CorrectionResultStatus::Rejected;
            return true;
        }
        return false;
    }
}

std::string stateToString(PlaybackState state)
{
    switch (state)
    {
    case PlaybackState::Playing:
        return "Playing";
    case PlaybackState::Paused:
        return "Paused";
    case PlaybackState::Stopped:
        return "Stopped";
    default:
        return "Unknown";
    }
}

std::string messageTypeToString(MessageType type)
{
    switch (type)
    {
    case MessageType::Play:
        return "PLAY";
    case MessageType::Pause:
        return "PAUSE";
    case MessageType::Seek:
        return "SEEK";
    case MessageType::Report:
        return "REPORT";
    case MessageType::Ping:
        return "PING";
    case MessageType::Pong:
        return "PONG";
    case MessageType::Join:
        return "JOIN";
    case MessageType::JoinRejected:
        return "REJECT";
    case MessageType::Snapshot:
        return "SNAPSHOT";
    case MessageType::Correction:
        return "CORRECT";
    case MessageType::CorrectionResult:
        return "CORRECT_RESULT";
    default:
        return "UNKNOWN";
    }
}

std::string correctionResultStatusToString(CorrectionResultStatus status)
{
    switch (status)
    {
    case CorrectionResultStatus::Applied:
        return "APPLIED";
    case CorrectionResultStatus::Rejected:
        return "REJECTED";
    case CorrectionResultStatus::None:
    default:
        return "NONE";
    }
}

std::string makeMediaIdentity(const std::string& mediaSource)
{
    std::string normalizedSource = mediaSource;
    std::replace(
        normalizedSource.begin(),
        normalizedSource.end(),
        '\\',
        '/'
    );

    // FNV-1a 64-bit 在 Windows/Linux 上结果一致，而且实现足够轻量。
    // 这里的目标是稳定标识，不是抵抗恶意碰撞。
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : normalizedSource)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

bool isPlaybackControlMessage(MessageType type)
{
    return type == MessageType::Play ||
        type == MessageType::Pause ||
        type == MessageType::Seek;
}

std::string messageToString(const SyncMessage& message)
{
    switch (message.type)
    {
    case MessageType::Play:
        if (message.controlEpoch > 0)
        {
            return "CONTROL " + std::to_string(message.controlEpoch) +
                " PLAY\n";
        }
        return "PLAY\n";
    case MessageType::Pause:
        if (message.controlEpoch > 0)
        {
            return "CONTROL " + std::to_string(message.controlEpoch) +
                " PAUSE\n";
        }
        return "PAUSE\n";
    case MessageType::Seek:
        if (message.controlEpoch > 0)
        {
            return "CONTROL " + std::to_string(message.controlEpoch) +
                " SEEK " + std::to_string(message.positionMilliseconds) +
                "\n";
        }
        return "SEEK " + std::to_string(message.positionSeconds) + "\n";
    case MessageType::Report:
        return "REPORT " + std::to_string(message.controlEpoch) +
            " " + std::to_string(message.positionMilliseconds) +
            " " + stateToString(message.playbackState) + "\n";
    case MessageType::Ping:
        return "PING " + std::to_string(message.sequenceNumber) + "\n";
    case MessageType::Pong:
        return "PONG " + std::to_string(message.sequenceNumber) + "\n";
    case MessageType::Join:
        return "JOIN " + message.mediaIdentity + "\n";
    case MessageType::JoinRejected:
        return "REJECT " + message.rejectionReason + "\n";
    case MessageType::Snapshot:
        return "SNAPSHOT " + std::to_string(message.controlEpoch) +
            " " + stateToString(message.playbackState) +
            " " + std::to_string(message.positionMilliseconds) +
            " " + message.mediaIdentity + "\n";
    case MessageType::Correction:
        return "CORRECT " + std::to_string(message.commandId) +
            " " + std::to_string(message.controlEpoch) +
            " " + stateToString(message.playbackState) +
            " " + std::to_string(message.correctionForwardMilliseconds) +
            "\n";
    case MessageType::CorrectionResult:
        return "CORRECT_RESULT " + std::to_string(message.commandId) +
            " " + std::to_string(message.controlEpoch) +
            " " + correctionResultStatusToString(
                message.correctionResultStatus
            ) +
            " " + std::to_string(message.positionMilliseconds) +
            " " + message.correctionReason + "\n";
    default:
        return "UNKNOWN\n";
    }
}

SyncMessage stringToMessage(const std::string& tcpString)
{
    std::istringstream iss(tcpString);
    std::string command;
    SyncMessage message;

    if (!(iss >> command))
    {
        return message;
    }

    if (command == "PLAY")
    {
        std::string extra;
        if (iss >> extra)
        {
            return message;
        }

        message.type = MessageType::Play;
        return message;
    }

    if (command == "CONTROL")
    {
        long long controlEpoch = 0;
        std::string controlType;
        std::string extra;
        if (!(iss >> controlEpoch) || controlEpoch <= 0 ||
            !(iss >> controlType))
        {
            return message;
        }

        if (controlType == "PLAY" || controlType == "PAUSE")
        {
            if (iss >> extra)
            {
                return message;
            }
            message.type = controlType == "PLAY"
                ? MessageType::Play
                : MessageType::Pause;
        }
        else if (controlType == "SEEK")
        {
            long long positionMilliseconds = 0;
            if (!(iss >> positionMilliseconds) ||
                !isValidPositionMilliseconds(positionMilliseconds) ||
                (iss >> extra))
            {
                return message;
            }
            message.type = MessageType::Seek;
            message.positionMilliseconds = positionMilliseconds;
            message.positionSeconds = static_cast<int>(
                positionMilliseconds / 1000
            );
        }
        else
        {
            return message;
        }

        message.controlEpoch = controlEpoch;
        return message;
    }

    if (command == "PAUSE")
    {
        std::string extra;
        if (iss >> extra)
        {
            return message;
        }

        message.type = MessageType::Pause;
        return message;
    }

    if (command == "SEEK")
    {
        int seconds = 0;
        std::string extra;

        if (!(iss >> seconds) || seconds < 0 || (iss >> extra))
        {
            return message;
        }

        message.type = MessageType::Seek;
        message.positionSeconds = seconds;
        message.positionMilliseconds = static_cast<long long>(seconds) * 1000;
        return message;
    }

    if (command == "REPORT")
    {
        long long controlEpoch = 0;
        long long milliseconds = 0;
        std::string stateText;
        std::string extra;
        PlaybackState playbackState = PlaybackState::Stopped;

        if (!(iss >> controlEpoch) || controlEpoch < 0 ||
            !(iss >> milliseconds) ||
            !isValidPositionMilliseconds(milliseconds) ||
            !(iss >> stateText) ||
            !stringToPlaybackState(stateText, playbackState) ||
            (iss >> extra))
        {
            return message;
        }

        message.type = MessageType::Report;
        message.controlEpoch = controlEpoch;
        message.positionMilliseconds = milliseconds;
        message.positionSeconds = static_cast<int>(milliseconds / 1000);
        message.playbackState = playbackState;
        return message;
    }

    if (command == "PING" || command == "PONG")
    {
        int sequenceNumber = 0;
        std::string extra;

        if (!(iss >> sequenceNumber) || sequenceNumber <= 0 || (iss >> extra))
        {
            return message;
        }

        message.type = command == "PING" ? MessageType::Ping : MessageType::Pong;
        message.sequenceNumber = sequenceNumber;
        return message;
    }

    if (command == "JOIN")
    {
        std::string mediaIdentity;
        std::string extra;
        if (!(iss >> mediaIdentity) ||
            !isValidMediaIdentity(mediaIdentity) ||
            (iss >> extra))
        {
            return message;
        }

        message.type = MessageType::Join;
        message.mediaIdentity = mediaIdentity;
        return message;
    }

    if (command == "REJECT")
    {
        std::string reason;
        std::string extra;
        if (!(iss >> reason) ||
            !isValidRejectionReason(reason) ||
            (iss >> extra))
        {
            return message;
        }

        message.type = MessageType::JoinRejected;
        message.rejectionReason = reason;
        return message;
    }

    if (command == "SNAPSHOT")
    {
        long long controlEpoch = 0;
        long long positionMilliseconds = 0;
        std::string stateText;
        std::string mediaIdentity;
        std::string extra;
        PlaybackState playbackState = PlaybackState::Stopped;

        if (!(iss >> controlEpoch) ||
            controlEpoch < 0 ||
            !(iss >> stateText) ||
            !stringToPlaybackState(stateText, playbackState) ||
            !(iss >> positionMilliseconds) ||
            !isValidPositionMilliseconds(positionMilliseconds) ||
            !(iss >> mediaIdentity) ||
            !isValidMediaIdentity(mediaIdentity) ||
            (iss >> extra))
        {
            return message;
        }

        message.type = MessageType::Snapshot;
        message.controlEpoch = controlEpoch;
        message.playbackState = playbackState;
        message.positionMilliseconds = positionMilliseconds;
        message.positionSeconds = static_cast<int>(positionMilliseconds / 1000);
        message.mediaIdentity = mediaIdentity;
        return message;
    }

    if (command == "CORRECT")
    {
        long long commandId = 0;
        long long controlEpoch = 0;
        long long forwardMilliseconds = 0;
        std::string stateText;
        std::string extra;
        PlaybackState playbackState = PlaybackState::Stopped;

        if (!(iss >> commandId) || commandId <= 0 ||
            !(iss >> controlEpoch) || controlEpoch <= 0 ||
            !(iss >> stateText) ||
            !stringToPlaybackState(stateText, playbackState) ||
            playbackState == PlaybackState::Stopped ||
            !(iss >> forwardMilliseconds) || forwardMilliseconds <= 0 ||
            !isValidPositionMilliseconds(forwardMilliseconds) ||
            (iss >> extra))
        {
            return message;
        }

        message.type = MessageType::Correction;
        message.commandId = commandId;
        message.controlEpoch = controlEpoch;
        message.playbackState = playbackState;
        message.correctionForwardMilliseconds = forwardMilliseconds;
        return message;
    }

    if (command == "CORRECT_RESULT")
    {
        long long commandId = 0;
        long long controlEpoch = 0;
        long long actualPositionMilliseconds = 0;
        std::string statusText;
        std::string reason;
        std::string extra;
        CorrectionResultStatus status = CorrectionResultStatus::None;

        if (!(iss >> commandId) || commandId <= 0 ||
            !(iss >> controlEpoch) || controlEpoch <= 0 ||
            !(iss >> statusText) ||
            !stringToCorrectionResultStatus(statusText, status) ||
            !(iss >> actualPositionMilliseconds) ||
            !isValidPositionMilliseconds(actualPositionMilliseconds) ||
            !(iss >> reason) || !isValidRejectionReason(reason) ||
            (status == CorrectionResultStatus::Applied && reason != "OK") ||
            (status == CorrectionResultStatus::Rejected && reason == "OK") ||
            (iss >> extra))
        {
            return message;
        }

        message.type = MessageType::CorrectionResult;
        message.commandId = commandId;
        message.controlEpoch = controlEpoch;
        message.correctionResultStatus = status;
        message.positionMilliseconds = actualPositionMilliseconds;
        message.positionSeconds = static_cast<int>(
            actualPositionMilliseconds / 1000
        );
        message.correctionReason = reason;
        return message;
    }

    return message;
}

void applyMessageToState(const SyncMessage& message, SyncState& state)
{
    switch (message.type)
    {
    case MessageType::Play:
        state.state = PlaybackState::Playing;
        break;
    case MessageType::Pause:
        state.state = PlaybackState::Paused;
        break;
    case MessageType::Seek:
        state.positionSeconds = message.positionSeconds;
        state.positionMilliseconds = message.positionMilliseconds;
        break;
    case MessageType::Report:
    case MessageType::Ping:
    case MessageType::Pong:
    case MessageType::Join:
    case MessageType::JoinRejected:
        break;
    case MessageType::Snapshot:
        state.state = message.playbackState;
        state.positionMilliseconds = message.positionMilliseconds;
        state.positionSeconds = static_cast<int>(message.positionMilliseconds / 1000);
        break;
    case MessageType::Correction:
    case MessageType::CorrectionResult:
    case MessageType::Unknown:
        break;
    }
}

std::string syncStateToString(const SyncState& state)
{
    return "state=" + stateToString(state.state) +
        ", position=" + std::to_string(state.positionSeconds) + " seconds" +
        ", positionMs=" + std::to_string(state.positionMilliseconds);
}

std::string stateResponseToString(const SyncState& state)
{
    return "STATE " + stateToString(state.state) +
        " " + std::to_string(state.positionSeconds) + "\n";
}

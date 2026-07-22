#include "Protocol.h"

#include <sstream>
#include <string>

namespace
{
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
    default:
        return "UNKNOWN";
    }
}

std::string messageToString(const SyncMessage& message)
{
    switch (message.type)
    {
    case MessageType::Play:
        return "PLAY\n";
    case MessageType::Pause:
        return "PAUSE\n";
    case MessageType::Seek:
        return "SEEK " + std::to_string(message.positionSeconds) + "\n";
    case MessageType::Report:
        return "REPORT " + std::to_string(message.positionMilliseconds) +
            " " + stateToString(message.playbackState) + "\n";
    case MessageType::Ping:
        return "PING " + std::to_string(message.sequenceNumber) + "\n";
    case MessageType::Pong:
        return "PONG " + std::to_string(message.sequenceNumber) + "\n";
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
        long long milliseconds = 0;
        std::string stateText;
        std::string extra;
        PlaybackState playbackState = PlaybackState::Stopped;

        if (!(iss >> milliseconds) ||
            milliseconds < 0 ||
            !(iss >> stateText) ||
            !stringToPlaybackState(stateText, playbackState) ||
            (iss >> extra))
        {
            return message;
        }

        message.type = MessageType::Report;
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
        break;
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

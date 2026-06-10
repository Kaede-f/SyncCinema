#include "Protocol.h"

#include <sstream>
#include <string>

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
        break;
    case MessageType::Unknown:
        break;
    }
}

std::string syncStateToString(const SyncState& state)
{
    return "state=" + stateToString(state.state) +
        ", position=" + std::to_string(state.positionSeconds) + " seconds";
}

std::string stateResponseToString(const SyncState& state)
{
    return "STATE " + stateToString(state.state) +
        " " + std::to_string(state.positionSeconds) + "\n";
}


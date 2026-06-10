#include "PlayerController.h"

PlayerController::~PlayerController() = default;

bool applyMessageToPlayer(const SyncMessage& message, PlayerController& player)
{
    switch (message.type)
    {
    case MessageType::Play:
        return player.play();
    case MessageType::Pause:
        return player.pause();
    case MessageType::Seek:
        return player.seek(message.positionSeconds);
    case MessageType::Unknown:
        return false;
    }

    return false;
}


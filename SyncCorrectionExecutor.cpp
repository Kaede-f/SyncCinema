#include "SyncCorrectionExecutor.h"

#include <algorithm>
#include <limits>

SyncCorrectionExecution executeSyncCorrection(
    const SyncMessage& command,
    long long currentControlEpoch,
    SyncState& localState,
    PlayerController& player)
{
    SyncCorrectionExecution execution;
    SyncMessage& result = execution.resultMessage;
    result.type = MessageType::CorrectionResult;
    result.commandId = command.commandId;
    result.controlEpoch = command.controlEpoch;
    result.correctionResultStatus = CorrectionResultStatus::Rejected;

    if (command.type != MessageType::Correction ||
        command.commandId <= 0 || command.controlEpoch <= 0)
    {
        result.correctionReason = "INVALID_COMMAND";
    }
    else if (command.controlEpoch != currentControlEpoch)
    {
        result.correctionReason = "EPOCH_MISMATCH";
    }
    else if (command.playbackState != localState.state)
    {
        result.correctionReason = "STATE_MISMATCH";
    }
    else if (!player.isSeekable())
    {
        result.correctionReason = "NOT_SEEKABLE";
    }
    else
    {
        long long currentPositionMs = player.getPositionMilliseconds();
        if (currentPositionMs < 0 ||
            command.correctionForwardMilliseconds <= 0 ||
            currentPositionMs > (std::numeric_limits<long long>::max)() -
                command.correctionForwardMilliseconds)
        {
            result.correctionReason = "INVALID_TARGET";
        }
        else
        {
            execution.requestedTargetMilliseconds = currentPositionMs +
                command.correctionForwardMilliseconds;
            long long durationMs = player.getDurationMilliseconds();
            if (durationMs > 0)
            {
                execution.requestedTargetMilliseconds = (std::min)(
                    execution.requestedTargetMilliseconds,
                    durationMs
                );
            }

            if (execution.requestedTargetMilliseconds <= currentPositionMs)
            {
                result.correctionReason = "INVALID_TARGET";
            }
            else if (!player.seekMilliseconds(
                    execution.requestedTargetMilliseconds))
            {
                result.correctionReason = "SEEK_FAILED";
            }
            else
            {
                execution.applied = true;
                result.correctionResultStatus =
                    CorrectionResultStatus::Applied;
                result.correctionReason = "OK";
            }
        }
    }

    result.positionMilliseconds = (std::max)(
        0LL,
        player.getPositionMilliseconds()
    );
    result.positionSeconds = static_cast<int>(
        result.positionMilliseconds / 1000
    );

    if (execution.applied)
    {
        localState.positionMilliseconds = result.positionMilliseconds;
        localState.positionSeconds = result.positionSeconds;
    }

    return execution;
}

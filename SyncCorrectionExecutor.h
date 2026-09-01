#pragma once

#include "PlayerController.h"
#include "Protocol.h"

struct SyncCorrectionExecution
{
    SyncMessage resultMessage;
    bool applied = false;
    long long requestedTargetMilliseconds = 0;
};

// 调用方必须在外层保护 player 和 localState；执行器本身不持有锁。
// 纯粹的输入/输出边界让真实 libVLC 与 MockPlayer 共用完全相同的安全规则。
SyncCorrectionExecution executeSyncCorrection(
    const SyncMessage& command,
    long long currentControlEpoch,
    SyncState& localState,
    PlayerController& player
);

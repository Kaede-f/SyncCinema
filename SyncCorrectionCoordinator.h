#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "Protocol.h"
#include "SyncCorrectionPolicy.h"

enum class CorrectionResultMatchStatus
{
    Matched,
    UnknownCommand,
    WrongClient,
    EpochMismatch,
    InvalidMessage
};

struct CorrectionResultRecord
{
    CorrectionResultMatchStatus matchStatus =
        CorrectionResultMatchStatus::InvalidMessage;
    int clientId = 0;
    int referenceClientId = 0;
    long long commandId = 0;
    long long controlEpoch = 0;
    long long forwardMilliseconds = 0;
    long long acknowledgementLatencyMs = -1;
    CorrectionResultStatus resultStatus = CorrectionResultStatus::None;
    long long actualPositionMilliseconds = 0;
    std::string reason;
};

// 协调器管理一条校正命令从“创建”到“收到回执”的生命周期。
// 它不访问 socket、Room 或播放器，因此命令匹配规则可以独立回归测试。
class SyncCorrectionCoordinator
{
public:
    using Clock = std::chrono::steady_clock;

    std::optional<SyncMessage> createCommand(
        const SyncCorrectionProposal& proposal,
        Clock::time_point now = Clock::now()
    );

    bool markDispatchFailed(long long commandId);

    CorrectionResultRecord recordResult(
        int clientId,
        const SyncMessage& result,
        Clock::time_point now = Clock::now()
    );

    void removeClient(int clientId);
    void retainControlEpoch(long long controlEpoch);
    std::size_t pendingCommandCount() const;

private:
    struct PendingCommand
    {
        SyncCorrectionProposal proposal;
        Clock::time_point sentAt{};
    };

    void pruneExpiredLocked(Clock::time_point now);

    mutable std::mutex mutex_;
    std::unordered_map<long long, PendingCommand> pendingByCommandId_;
    long long nextCommandId_ = 1;
};

const char* correctionResultMatchStatusToString(
    CorrectionResultMatchStatus status
);

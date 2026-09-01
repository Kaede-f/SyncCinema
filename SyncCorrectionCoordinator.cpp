#include "SyncCorrectionCoordinator.h"

#include <algorithm>
#include <limits>

namespace
{
    constexpr auto kPendingCommandTimeout = std::chrono::seconds(10);
    constexpr std::size_t kMaxPendingCommands = 256;
}

std::optional<SyncMessage> SyncCorrectionCoordinator::createCommand(
    const SyncCorrectionProposal& proposal,
    Clock::time_point now)
{
    if (proposal.targetClientId <= 0 ||
        proposal.referenceClientId <= 0 ||
        proposal.targetClientId == proposal.referenceClientId ||
        proposal.controlEpoch <= 0 ||
        proposal.playbackState == PlaybackState::Stopped ||
        proposal.forwardMilliseconds <= 0)
    {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pruneExpiredLocked(now);

    if (pendingByCommandId_.size() >= kMaxPendingCommands ||
        nextCommandId_ <= 0 ||
        nextCommandId_ == std::numeric_limits<long long>::max())
    {
        return std::nullopt;
    }

    for (const auto& [commandId, pending] : pendingByCommandId_)
    {
        (void)commandId;
        if (pending.proposal.targetClientId == proposal.targetClientId)
        {
            // ACK 丢失时，不能让同一 client 同时执行两条独立 seek。
            // 旧命令超时清理后才允许重试。
            return std::nullopt;
        }
    }

    const long long commandId = nextCommandId_++;
    pendingByCommandId_[commandId] = PendingCommand{ proposal, now };

    SyncMessage command;
    command.type = MessageType::Correction;
    command.commandId = commandId;
    command.controlEpoch = proposal.controlEpoch;
    command.playbackState = proposal.playbackState;
    command.correctionForwardMilliseconds = proposal.forwardMilliseconds;
    return command;
}

bool SyncCorrectionCoordinator::markDispatchFailed(long long commandId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pendingByCommandId_.erase(commandId) > 0;
}

CorrectionResultRecord SyncCorrectionCoordinator::recordResult(
    int clientId,
    const SyncMessage& result,
    Clock::time_point now)
{
    CorrectionResultRecord record;
    record.clientId = clientId;
    record.commandId = result.commandId;
    record.controlEpoch = result.controlEpoch;
    record.resultStatus = result.correctionResultStatus;
    record.actualPositionMilliseconds = result.positionMilliseconds;
    record.reason = result.correctionReason;

    if (clientId <= 0 || result.type != MessageType::CorrectionResult ||
        result.commandId <= 0 || result.controlEpoch <= 0 ||
        result.correctionResultStatus == CorrectionResultStatus::None)
    {
        return record;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pruneExpiredLocked(now);

    auto pendingIt = pendingByCommandId_.find(result.commandId);
    if (pendingIt == pendingByCommandId_.end())
    {
        record.matchStatus = CorrectionResultMatchStatus::UnknownCommand;
        return record;
    }

    const SyncCorrectionProposal& proposal = pendingIt->second.proposal;
    record.referenceClientId = proposal.referenceClientId;
    record.forwardMilliseconds = proposal.forwardMilliseconds;

    // 错误来源或旧 epoch 的回执不能消费 pending 命令；
    // 合法 client 之后仍有机会返回真正的结果。
    if (clientId != proposal.targetClientId)
    {
        record.matchStatus = CorrectionResultMatchStatus::WrongClient;
        return record;
    }

    if (result.controlEpoch != proposal.controlEpoch)
    {
        record.matchStatus = CorrectionResultMatchStatus::EpochMismatch;
        return record;
    }

    record.matchStatus = CorrectionResultMatchStatus::Matched;
    long long acknowledgementLatencyMs = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - pendingIt->second.sentAt
        ).count()
    );
    record.acknowledgementLatencyMs = (std::max)(
        0LL,
        acknowledgementLatencyMs
    );
    pendingByCommandId_.erase(pendingIt);
    return record;
}

void SyncCorrectionCoordinator::removeClient(int clientId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = pendingByCommandId_.begin();
        it != pendingByCommandId_.end();)
    {
        if (it->second.proposal.targetClientId == clientId ||
            it->second.proposal.referenceClientId == clientId)
        {
            it = pendingByCommandId_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void SyncCorrectionCoordinator::retainControlEpoch(long long controlEpoch)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = pendingByCommandId_.begin();
        it != pendingByCommandId_.end();)
    {
        if (it->second.proposal.controlEpoch != controlEpoch)
        {
            it = pendingByCommandId_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::size_t SyncCorrectionCoordinator::pendingCommandCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pendingByCommandId_.size();
}

void SyncCorrectionCoordinator::pruneExpiredLocked(Clock::time_point now)
{
    for (auto it = pendingByCommandId_.begin();
        it != pendingByCommandId_.end();)
    {
        if (now - it->second.sentAt >= kPendingCommandTimeout)
        {
            it = pendingByCommandId_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

const char* correctionResultMatchStatusToString(
    CorrectionResultMatchStatus status)
{
    switch (status)
    {
    case CorrectionResultMatchStatus::Matched:
        return "matched";
    case CorrectionResultMatchStatus::UnknownCommand:
        return "unknown_command";
    case CorrectionResultMatchStatus::WrongClient:
        return "wrong_client";
    case CorrectionResultMatchStatus::EpochMismatch:
        return "epoch_mismatch";
    case CorrectionResultMatchStatus::InvalidMessage:
    default:
        return "invalid_message";
    }
}

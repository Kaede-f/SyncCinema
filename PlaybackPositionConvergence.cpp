#include "PlaybackPositionConvergence.h"

#include <algorithm>
#include <limits>

namespace
{
    long long elapsedMilliseconds(
        PlaybackPositionConvergenceTracker::Clock::time_point earlier,
        PlaybackPositionConvergenceTracker::Clock::time_point later)
    {
        if (later <= earlier)
        {
            return 0;
        }

        return std::chrono::duration_cast<std::chrono::milliseconds>(
            later - earlier
        ).count();
    }

    long long addWithoutOverflow(long long value, long long increment)
    {
        if (increment > 0 &&
            value > (std::numeric_limits<long long>::max)() - increment)
        {
            return (std::numeric_limits<long long>::max)();
        }

        return value + increment;
    }
}

PlaybackPositionConvergenceTracker::PlaybackPositionConvergenceTracker(
    long long targetPositionMilliseconds,
    PlaybackState playbackState,
    Clock::time_point targetReferenceTime,
    PlaybackPositionConvergenceConfig config)
    : targetPositionMilliseconds_(std::max(0LL, targetPositionMilliseconds)),
      playbackState_(playbackState),
      targetReferenceTime_(targetReferenceTime),
      config_(config)
{
    config_.toleranceMilliseconds =
        std::max(0LL, config_.toleranceMilliseconds);
    config_.requiredConsecutiveSamples =
        std::max(1, config_.requiredConsecutiveSamples);
}

PlaybackPositionObservation PlaybackPositionConvergenceTracker::observe(
    long long actualPositionMilliseconds,
    Clock::time_point observedAt)
{
    PlaybackPositionObservation observation;
    observation.actualPositionMilliseconds =
        std::max(0LL, actualPositionMilliseconds);
    observation.expectedPositionMilliseconds = targetPositionMilliseconds_;

    // Playing 状态下目标位置也在继续向前，因此不能始终拿最初的 seek
    // 目标比较。Paused/Stopped 的目标位置保持不变。
    if (playbackState_ == PlaybackState::Playing)
    {
        observation.expectedPositionMilliseconds = addWithoutOverflow(
            targetPositionMilliseconds_,
            elapsedMilliseconds(targetReferenceTime_, observedAt)
        );
    }

    observation.errorMilliseconds =
        observation.actualPositionMilliseconds -
        observation.expectedPositionMilliseconds;

    const long long absoluteError =
        observation.actualPositionMilliseconds >=
            observation.expectedPositionMilliseconds
            ? observation.actualPositionMilliseconds -
                observation.expectedPositionMilliseconds
            : observation.expectedPositionMilliseconds -
                observation.actualPositionMilliseconds;

    if (absoluteError <= config_.toleranceMilliseconds)
    {
        ++consecutiveSamples_;
    }
    else
    {
        // 单次碰巧落入容差不代表播放器已经稳定；任何越界样本都会
        // 重新开始计数。
        consecutiveSamples_ = 0;
    }

    observation.consecutiveSamples = consecutiveSamples_;
    observation.converged =
        consecutiveSamples_ >= config_.requiredConsecutiveSamples;
    return observation;
}

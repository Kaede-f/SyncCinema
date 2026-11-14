#include "RttEstimator.h"

#include <algorithm>
#include <numeric>
#include <vector>

namespace
{
    long long nearestRankPercentile(
        const std::vector<long long>& sortedValues,
        std::size_t percentile)
    {
        if (sortedValues.empty())
        {
            return -1;
        }

        const std::size_t rank =
            (sortedValues.size() * percentile + 99) / 100;
        return sortedValues[rank - 1];
    }

    long long medianOfSorted(const std::vector<long long>& sortedValues)
    {
        const std::size_t middle = sortedValues.size() / 2;
        if (sortedValues.size() % 2 == 1)
        {
            return sortedValues[middle];
        }

        const long long lower = sortedValues[middle - 1];
        const long long upper = sortedValues[middle];
        return lower + (upper - lower) / 2;
    }
}

RttEstimator::RttEstimator(std::size_t windowCapacity)
    : windowCapacity_(std::max<std::size_t>(1, windowCapacity))
{
}

RttStatistics RttEstimator::addSample(long long rttUs)
{
    if (rttUs < 0)
    {
        return snapshot();
    }

    ++totalSamples_;
    latestUs_ = rttUs;
    lifetimeMinUs_ = totalSamples_ == 1
        ? rttUs
        : std::min(lifetimeMinUs_, rttUs);
    lifetimeMaxUs_ = totalSamples_ == 1
        ? rttUs
        : std::max(lifetimeMaxUs_, rttUs);

    recentSamplesUs_.push_back(rttUs);
    if (recentSamplesUs_.size() > windowCapacity_)
    {
        recentSamplesUs_.pop_front();
    }

    return snapshot();
}

RttStatistics RttEstimator::snapshot() const
{
    RttStatistics result;
    result.totalSamples = totalSamples_;
    result.windowSamples = recentSamplesUs_.size();
    result.latestUs = latestUs_;
    result.lifetimeMinUs = lifetimeMinUs_;
    result.lifetimeMaxUs = lifetimeMaxUs_;

    if (recentSamplesUs_.empty())
    {
        return result;
    }

    std::vector<long long> sortedValues(
        recentSamplesUs_.begin(),
        recentSamplesUs_.end()
    );
    std::sort(sortedValues.begin(), sortedValues.end());

    const long long total = std::accumulate(
        sortedValues.begin(),
        sortedValues.end(),
        0LL
    );
    result.rollingAverageUs = total / static_cast<long long>(sortedValues.size());
    result.rollingMedianUs = medianOfSorted(sortedValues);
    result.rollingP95Us = nearestRankPercentile(sortedValues, 95);

    // P10 比 lifetime minimum 更能代表当前链路的低排队延迟，
    // 同时不会因很久以前的一次低值永远固定住补偿基线。
    result.baselineUs = nearestRankPercentile(sortedValues, 10);
    return result;
}

void RttEstimator::clear()
{
    recentSamplesUs_.clear();
    totalSamples_ = 0;
    latestUs_ = -1;
    lifetimeMinUs_ = -1;
    lifetimeMaxUs_ = -1;
}

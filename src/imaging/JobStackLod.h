#pragma once

#include "domain/JobModels.h"
#include "domain/Models.h"

#include <cstddef>
#include <deque>
#include <vector>

class JobStackLod final {
public:
    [[nodiscard]] static std::vector<JobStackHeightLevel> buildPyramid(
        const QSize& baseSize, const QVector<quint16>& baseLevels,
        const CancellationTokenPtr& token);
    [[nodiscard]] static int chooseLevel(const std::vector<JobStackHeightLevel>& levels,
                                         double projectedWidth, double projectedHeight,
                                         bool interacting, int qualityOffset = 0);
};

class JobFrameBudgetController final {
public:
    static constexpr qint64 kInitialTriangleBudget = 1'500'000;
    static constexpr qint64 kMinimumTriangleBudget = 100'000;
    static constexpr qint64 kMaximumTriangleBudget = 6'000'000;

    void reset();
    void recordFrame(double milliseconds, bool interacting);
    [[nodiscard]] qint64 triangleBudget() const noexcept { return triangleBudget_; }
    [[nodiscard]] double averageFrameMilliseconds() const noexcept { return averageMs_; }
    [[nodiscard]] double framesPerSecond() const noexcept {
        return averageMs_ > 0.0 ? 1000.0 / averageMs_ : 0.0;
    }
    [[nodiscard]] int qualityOffset() const noexcept { return qualityOffset_; }

private:
    std::deque<double> samples_;
    qint64 triangleBudget_ = kInitialTriangleBudget;
    double averageMs_ = 0.0;
    int slowFrames_ = 0;
    int fastFrames_ = 0;
    int qualityOffset_ = 0;
};

#include "imaging/JobStackLod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace {
JobStackHeightLevel reduceTo(const QSize& sourceSize, const std::vector<quint16>& source,
                             int maximumSide, const CancellationTokenPtr& token) {
    const double scale = std::min({1.0, double(maximumSide) / sourceSize.width(),
                                  double(maximumSide) / sourceSize.height()});
    JobStackHeightLevel result;
    result.gridSize = {std::max(1, qRound(sourceSize.width() * scale)),
                       std::max(1, qRound(sourceSize.height() * scale))};
    result.columnLayers.resize(static_cast<std::size_t>(result.gridSize.width()) *
                               result.gridSize.height());
    for (int y = 0; y < result.gridSize.height(); ++y) {
        if (token && token->isCancelled()) return {};
        const int beginY = y * sourceSize.height() / result.gridSize.height();
        const int endY = std::max(beginY + 1,
            (y + 1) * sourceSize.height() / result.gridSize.height());
        for (int x = 0; x < result.gridSize.width(); ++x) {
            const int beginX = x * sourceSize.width() / result.gridSize.width();
            const int endX = std::max(beginX + 1,
                (x + 1) * sourceSize.width() / result.gridSize.width());
            quint16 maximum = 0;
            for (int sourceY = beginY; sourceY < endY; ++sourceY)
                for (int sourceX = beginX; sourceX < endX; ++sourceX)
                    maximum = std::max(maximum, source[static_cast<std::size_t>(sourceY) *
                                                       sourceSize.width() + sourceX]);
            result.columnLayers[static_cast<std::size_t>(y) * result.gridSize.width() + x] = maximum;
        }
    }
    return result;
}
}

std::vector<JobStackHeightLevel> JobStackLod::buildPyramid(
    const QSize& baseSize, const QVector<quint16>& baseLevels,
    const CancellationTokenPtr& token) {
    std::vector<JobStackHeightLevel> result;
    if (baseSize.width() <= 0 || baseSize.height() <= 0 ||
        baseLevels.size() != qsizetype(baseSize.width()) * baseSize.height()) return result;
    const std::vector<quint16> source(baseLevels.cbegin(), baseLevels.cend());
    static constexpr std::array<int, 5> sides{32, 64, 128, 256, 512};
    QSize previous;
    for (int side : sides) {
        JobStackHeightLevel level = reduceTo(baseSize, source, side, token);
        if (level.gridSize.isEmpty() || (token && token->isCancelled())) return {};
        if (level.gridSize != previous) {
            previous = level.gridSize;
            result.push_back(std::move(level));
        }
        if (previous == baseSize) break;
    }
    return result;
}

int JobStackLod::chooseLevel(const std::vector<JobStackHeightLevel>& levels,
                             double projectedWidth, double projectedHeight,
                             bool interacting, int qualityOffset) {
    if (levels.empty()) return -1;
    const double requiredSide = std::max(projectedWidth, projectedHeight) /
                                (interacting ? 1.5 : 0.75);
    int selected = static_cast<int>(levels.size()) - 1;
    for (int index = 0; index < static_cast<int>(levels.size()); ++index) {
        const QSize size = levels[static_cast<std::size_t>(index)].gridSize;
        if (std::max(size.width(), size.height()) >= requiredSide) {
            selected = index;
            break;
        }
    }
    return std::clamp(selected + qualityOffset, 0, static_cast<int>(levels.size()) - 1);
}

void JobFrameBudgetController::reset() {
    samples_.clear();
    triangleBudget_ = kInitialTriangleBudget;
    averageMs_ = 0.0;
    slowFrames_ = fastFrames_ = qualityOffset_ = 0;
}

void JobFrameBudgetController::recordFrame(double milliseconds, bool interacting) {
    if (!std::isfinite(milliseconds) || milliseconds <= 0.0) return;
    milliseconds = std::min(milliseconds, 250.0);
    samples_.push_back(milliseconds);
    if (samples_.size() > 12) samples_.pop_front();
    averageMs_ = std::accumulate(samples_.cbegin(), samples_.cend(), 0.0) / samples_.size();
    if (milliseconds > 33.3) {
        triangleBudget_ = std::max(kMinimumTriangleBudget,
            static_cast<qint64>(triangleBudget_ * 0.65));
        qualityOffset_ = std::max(-4, qualityOffset_ - 1);
        slowFrames_ = fastFrames_ = 0;
        return;
    }
    if (averageMs_ > 16.7) {
        if (++slowFrames_ >= 3) {
            triangleBudget_ = std::max(kMinimumTriangleBudget,
                static_cast<qint64>(triangleBudget_ * 0.65));
            qualityOffset_ = std::max(-4, qualityOffset_ - 1);
            slowFrames_ = fastFrames_ = 0;
        }
    } else {
        slowFrames_ = 0;
        if (!interacting && averageMs_ < 12.0 && ++fastFrames_ >= 30) {
            triangleBudget_ = std::min(kMaximumTriangleBudget,
                static_cast<qint64>(triangleBudget_ * 1.2));
            qualityOffset_ = std::min(0, qualityOffset_ + 1);
            fastFrames_ = 0;
        } else if (interacting || averageMs_ >= 12.0) {
            fastFrames_ = 0;
        }
    }
}

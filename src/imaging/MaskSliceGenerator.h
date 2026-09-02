#pragma once

#include "domain/MaskSliceModels.h"

#include <functional>

class MaskSliceGenerator {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    [[nodiscard]] static MaskSliceAnalysis analyze(
        const MaskSliceRequest& request, const CancellationTokenPtr& token,
        const Progress& progress, QString* error);

    [[nodiscard]] static MaskStackGroupAnalysis analyzeStackGroup(
        const MaskStackGroupRequest& request, const CancellationTokenPtr& token,
        const Progress& progress, QString* error);

    [[nodiscard]] static int layerCount(double actualMaximumHeight,
                                        double configuredMaximumHeight,
                                        double layerHeight, QString* error);
};

#pragma once

#include "domain/Models.h"
#include <functional>

class PatternDetector {
public:
    using Progress = std::function<void(const TaskProgress&)>;
    static SegmentationResult detect(const ImageDocumentPtr& document,
                                     const CancellationTokenPtr& token,
                                     const Progress& progress, QString* error);
    static SegmentationResult makeGrid(const ImageDocumentPtr& document,
                                       const SegmentationSettings& settings);
};


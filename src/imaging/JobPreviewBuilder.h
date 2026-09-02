#pragma once

#include "domain/JobModels.h"
#include "domain/Models.h"
#include "io/SplitExporter.h"

class JobPreviewBuilder {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    static JobDocumentPtr build(const ImageMetadata& metadata,
                                const SegmentationResult& segmentation,
                                TraversalMode traversalMode,
                                const PrintExportOptions& options,
                                const CancellationTokenPtr& token,
                                const Progress& progress, QString* error);
    static JobDocumentPtr build(const ImageMetadata& metadata,
                                const SegmentationResult& segmentation,
                                TraversalMode traversalMode,
                                const JobPlacementOptions& options,
                                const CancellationTokenPtr& token,
                                const Progress& progress, QString* error);
};

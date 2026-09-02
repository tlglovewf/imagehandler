#pragma once

#include "domain/Models.h"
#include "domain/PointCloudModels.h"

#include <QMetaType>
#include <QString>

enum class LayerCountStrategy { RoundUp, RequireExact };

struct JobPlacementOptions {
    double pixelScale = 0.25;
    bool reverseY = false;
};

struct LayeredPackageOptions {
    double pixelScale = 0.25;
    double maximumHeight = 12.5;
    double layerHeight = 0.1;
    int outputWidth = 0;
    int outputHeight = 0;
    bool invertHeight = true;
    bool invertMask = false;
    bool reverseY = false;
    LayerCountStrategy layerCountStrategy = LayerCountStrategy::RoundUp;
};

struct LayeredPackageRequest {
    QString outputDirectory;
    ImageDocumentPtr document;
    SegmentationResult segmentation;
    TraversalMode traversalMode = TraversalMode::SnakeZ;
    LayeredPackageOptions options;
    bool writeReport = true;
};

Q_DECLARE_METATYPE(LayerCountStrategy)
Q_DECLARE_METATYPE(LayeredPackageRequest)

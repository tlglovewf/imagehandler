#pragma once

#include "domain/Models.h"
#include "domain/PointCloudModels.h"

#include <QMetaType>
#include <QString>

enum class LayerCountStrategy { RoundUp, RequireExact };
enum class BinaryTransformSampling { Nearest, SmoothThreshold };

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
    bool attachToCanvas = true;
    int canvasWidth = 1920;
    int canvasHeight = 1080;
    double rotationDegrees = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    BinaryTransformSampling transformSampling = BinaryTransformSampling::Nearest;
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
Q_DECLARE_METATYPE(BinaryTransformSampling)
Q_DECLARE_METATYPE(LayeredPackageRequest)

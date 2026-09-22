#pragma once

#include "domain/Models.h"
#include "domain/PointCloudModels.h"
#include "imaging/ElementGrouping.h"

#include <QMetaType>
#include <QPoint>
#include <QString>
#include <QVector>

enum class LayerCountStrategy { RoundUp, RequireExact };
enum class BinaryTransformSampling { Nearest, SmoothThreshold };
enum class JobMoveCommand { RelMove, AbsMove };

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
    int baseLayerCount = 0;
    bool canvasBackgroundWhite = true;
    bool invertCanvas = false;
    double zOffset = 0.001;
    JobMoveCommand jobMoveCommand = JobMoveCommand::AbsMove;
    double exportMultiplier = 13.55;
    bool mirrorX = false;
};

struct LayeredPackageRequest {
    QString outputDirectory;
    ImageDocumentPtr document;
    SegmentationResult segmentation;
    TraversalMode traversalMode = TraversalMode::SnakeZ;
    LayeredPackageOptions options;
    bool writeReport = true;
    ElementGroupingResult grouping;
    // Zero or one selected cell keeps the historical "export all" behavior.
    // Two or more cells restrict both placements and generated models.
    QVector<QPoint> selectedCells;
};

Q_DECLARE_METATYPE(LayerCountStrategy)
Q_DECLARE_METATYPE(BinaryTransformSampling)
Q_DECLARE_METATYPE(JobMoveCommand)
Q_DECLARE_METATYPE(LayeredPackageRequest)

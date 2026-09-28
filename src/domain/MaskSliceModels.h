#pragma once

#include "domain/PointCloudModels.h"

#include <QImage>
#include <QMetaType>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QVector>
#include <cstdint>
#include <memory>
#include <vector>

inline constexpr int kMaximumMaskSliceLayers = 1000;

struct MaskSliceSettings {
    double layerHeight = 0.1;
    int outputWidth = 0;
    int outputHeight = 0;
    bool invertMask = false;
    double zOffset = 0.001;
};

struct MaskSliceRequest {
    ImageDocumentPtr source;
    ImageRegion region;
    PointCloudSettings mapping;
    MaskSliceSettings settings;
    quint64 requestGeneration = 0;
    bool generateStackPreview = true;
};

struct MaskSliceElement {
    ImageRegion region;
    int row = -1;
    int column = -1;
    qint64 traversalIndex = -1;
};

struct MaskStackGroupRequest {
    ImageDocumentPtr source;
    QVector<MaskSliceElement> elements;
    PointCloudSettings mapping;
    double layerHeight = 0.1;
    double outputScaleX = 1.0;
    double outputScaleY = 1.0;
    quint64 requestGeneration = 0;
    double zOffset = 0.001;
};

struct MaskStackBlockPlacement {
    QRectF physicalRect;
    qint64 traversalIndex = -1;
    int placementNumber = 0;
    QPointF jobPosition;
};

struct MaskStackVertex {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float normalX = 0.0F;
    float normalY = 0.0F;
    float normalZ = 1.0F;
    float layer = 0.0F;
    float faceKind = 0.0F;
};

struct MaskStackGeometry {
    std::vector<MaskStackVertex> vertices;
    std::vector<std::uint32_t> indices;
    int layerStride = 1;
};

struct MaskStackPreviewData {
    QSize gridSize;
    QSize requestedOutputSize;
    QSizeF physicalSize;
    QVector<quint16> columnLayers;
    double layerHeight = 0.0;
    int layerCount = 0;
    bool downsampled = false;
    // Normal stack previews map raster row 0 to the positive-Y edge. Processing
    // playback disables this so mask rows retain their native Y order.
    bool mapRasterYToPositiveY = true;
    double actualMaximumHeight = 0.0;
    QVector<MaskStackBlockPlacement> blocks;
    // Local preview-space position of the physical Job coordinate origin.
    // Multi-placement export previews use this to draw the real X/Y axes after
    // their geometry has been translated into a compact local bounding box.
    QPointF jobCoordinateOrigin;
    bool hasJobCoordinates = false;
    std::shared_ptr<const MaskStackGeometry> solidGeometry;
    std::shared_ptr<const MaskStackGeometry> explodedGeometry;
};

using MaskStackPreviewDataPtr = std::shared_ptr<const MaskStackPreviewData>;

struct MaskSliceAnalysis {
    quint64 requestGeneration = 0;
    double actualMaximumHeight = 0.0;
    int layerCount = 0;
    QSize sourceSize;
    QSize previewSize;
    QVector<QImage> thumbnails;
    MaskStackPreviewDataPtr stackPreview;
};

struct MaskStackGroupAnalysis {
    quint64 requestGeneration = 0;
    MaskStackPreviewDataPtr preview;
    struct ElementLayers {
        qint64 traversalIndex = -1;
        double actualMaximumHeight = 0.0;
        int layerCount = 0;
    };
    QVector<ElementLayers> elements;
    int commonLayerCount = 0;
};

Q_DECLARE_METATYPE(MaskSliceAnalysis)
Q_DECLARE_METATYPE(MaskStackGroupAnalysis)

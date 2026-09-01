#pragma once

#include "domain/PointCloudModels.h"

#include <QImage>
#include <QMetaType>
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
};

struct MaskSliceRequest {
    ImageDocumentPtr source;
    ImageRegion region;
    PointCloudSettings mapping;
    MaskSliceSettings settings;
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
    std::shared_ptr<const MaskStackGeometry> solidGeometry;
    std::shared_ptr<const MaskStackGeometry> explodedGeometry;
};

using MaskStackPreviewDataPtr = std::shared_ptr<const MaskStackPreviewData>;

struct MaskSliceAnalysis {
    double actualMaximumHeight = 0.0;
    int layerCount = 0;
    QSize sourceSize;
    QSize previewSize;
    QVector<QImage> thumbnails;
    MaskStackPreviewDataPtr stackPreview;
};

Q_DECLARE_METATYPE(MaskSliceAnalysis)

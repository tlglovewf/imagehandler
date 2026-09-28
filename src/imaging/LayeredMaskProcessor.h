#pragma once

#include "domain/MaskSliceModels.h"
#include "io/LayeredPackageTypes.h"

#include <cstddef>
#include <functional>
#include <QPointF>

struct LayeredMaskPlacement {
    ImageRegion modelRegion;
    QPoint cell;
    QPointF jobPosition;
    qint64 traversalIndex = -1;
};

struct LayeredMaskRequest {
    ImageDocumentPtr source;
    ImageRegion region;
    QSize nominalSourceSize;
    LayeredPackageOptions options;
    int layerCount = 0;
    quint64 requestGeneration = 0;
    QVector<LayeredMaskPlacement> placements;
    int activePlacement = 0;
    bool includeThumbnails = true;
    int stackMaximumSide = 512;
    std::size_t maximumExplodedQuads = 2'000'000;
};

struct LayeredMaskPreview {
    quint64 requestGeneration = 0;
    QSize outputSize;
    QSize contentSize;
    double actualMaximumHeight = 0.0;
    QVector<QImage> thumbnails;
    MaskStackPreviewDataPtr stackPreview;
};

struct LayeredMaskExportMetrics {
    qint64 transformedPixelCount = 0;
    qint64 packedPixelCount = 0;
};

Q_DECLARE_METATYPE(LayeredMaskPreview)

class LayeredMaskProcessor final {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    [[nodiscard]] static QSize outputSize(const LayeredPackageOptions& options);
    [[nodiscard]] static QSize contentSize(const LayeredMaskRequest& request);
    [[nodiscard]] static bool validate(const LayeredMaskRequest& request,
                                       QString* error = nullptr);
    [[nodiscard]] static quint64 bmpFileSize(const QSize& size);
    [[nodiscard]] static LayeredMaskPreview preview(
        const LayeredMaskRequest& request, const CancellationTokenPtr& token,
        const Progress& progress = {}, QString* error = nullptr);
    [[nodiscard]] static bool exportAll(
        const QString& outputDirectory, const LayeredMaskRequest& request,
        const CancellationTokenPtr& token, const Progress& progress,
        QString* error, LayeredMaskExportMetrics* metrics = nullptr);
};

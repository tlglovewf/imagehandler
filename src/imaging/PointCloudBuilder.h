#pragma once

#include "domain/PointCloudModels.h"

#include <functional>

class PointCloudBuilder final {
public:
    [[nodiscard]] static int rootLevelFor(const ImageMetadata& metadata);
    [[nodiscard]] static PointCloudDocumentPtr build(
        const ImageDocumentPtr& source, const PointCloudSettings& settings, quint64 generation,
        const CancellationTokenPtr& token, const std::function<void(const TaskProgress&)>& progress,
        QString* error);
    [[nodiscard]] static PointCloudTilePtr buildTile(
        const PointCloudDocumentPtr& document, int level, int tileX, int tileY,
        const CancellationTokenPtr& token, QString* error);
};

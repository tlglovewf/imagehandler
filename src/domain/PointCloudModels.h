#pragma once

#include "domain/Models.h"

#include <QColor>
#include <QMetaType>
#include <memory>
#include <optional>
#include <vector>

inline constexpr int kPointCloudTileSamples = 256;
inline constexpr qint64 kPointCloudRootPointBudget = 1'000'000;
inline constexpr qint64 kPointCloudFramePointBudget = 2'000'000;

struct PointCloudSettings {
    double pixelScale = 0.25;
    double maximumHeight = 12.5;
    bool invertHeight = true;
    QColor startColor{0, 0, 255};
    QColor endColor{0, 255, 0};
};

struct PointCloudVertex {
    float pixelX = 0.0F;
    float pixelY = 0.0F;
    float normalizedGray = 0.0F;
};

struct PointCloudExtremum {
    qint64 pixelX = 0;
    qint64 pixelY = 0;
    double normalizedGray = 0.0;
};

struct PointCloudTile {
    int level = 0;
    int tileX = 0;
    int tileY = 0;
    std::vector<PointCloudVertex> vertices;
};
using PointCloudTilePtr = std::shared_ptr<PointCloudTile>;

struct PointCloudDocument {
    ImageDocumentPtr source;
    PointCloudSettings settings;
    int rootLevel = 0;
    quint64 generation = 0;
    std::vector<PointCloudTilePtr> rootTiles;
    std::optional<PointCloudExtremum> highestPoint;
    std::optional<PointCloudExtremum> lowestPoint;
};
using PointCloudDocumentPtr = std::shared_ptr<PointCloudDocument>;

Q_DECLARE_METATYPE(PointCloudDocumentPtr)

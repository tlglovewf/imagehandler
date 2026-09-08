#include "imaging/JobPreviewBuilder.h"
#include "imaging/JobSpatialIndex.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <new>

namespace {
JobDocumentPtr buildDocument(const ImageMetadata& metadata,
                             const SegmentationResult& segmentation,
                             TraversalMode traversalMode,
                             const CancellationTokenPtr& token,
                             const JobPreviewBuilder::Progress& progress, QString* error,
                             const std::function<QPointF(const QPoint&)>& positionFor,
                             const ElementGroupingResult* grouping) {
    if (error) error->clear();
    if (!token || metadata.width <= 0 || metadata.height <= 0 ||
        segmentation.rows <= 0 || segmentation.columns <= 0 ||
        segmentation.settings.cellWidth <= 0 || segmentation.settings.cellHeight <= 0) {
        if (error) *error = QStringLiteral("无法从当前分割参数生成 Job 预览。");
        return {};
    }
    const qint64 count = qint64(segmentation.rows) * segmentation.columns;
    if (count <= 0 || count > kMaximumSegmentationCells) {
        if (error) *error = QStringLiteral("Job 预览点数无效或超过 %1。").arg(kMaximumSegmentationCells);
        return {};
    }
    const auto order = SplitExporter::traversal(segmentation.rows, segmentation.columns,
                                                 traversalMode);
    if (order.size() != static_cast<size_t>(count)) {
        if (error) *error = QStringLiteral("无法生成完整的分割遍历顺序。");
        return {};
    }
    try {
        auto document = std::make_shared<JobDocument>();
        document->source = JobDocumentSource::GeneratedPreview;
        const bool grouped = grouping && grouping->valid &&
            grouping->rows == segmentation.rows && grouping->columns == segmentation.columns;
        const qint64 modelCount = grouped ? grouping->groups.size() : count;
        document->models.reserve(static_cast<size_t>(modelCount));
        document->points.reserve(static_cast<size_t>(count));
        document->linkedCells.resize(static_cast<size_t>(count));
        for (qint64 model = 0; model < modelCount; ++model)
            document->models.push_back({model, QString::number(model), QString::number(model)});
        qint64 lastPermille = -1;
        for (qint64 i = 0; i < count; ++i) {
            if (token->isCancelled()) return {};
            const QPoint cell = order[static_cast<size_t>(i)];
            const QPointF position = positionFor(cell);
            const JobCoordinate xyz{position.x(), position.y(), 0.0};
            const int groupId = grouped ? grouping->groupIdAt(cell.y(), cell.x())
                                        : static_cast<int>(i);
            const QColor color = grouped ? grouping->colorAt(cell.y(), cell.x()) : QColor{};
            document->points.push_back({groupId, xyz, i * 2 + 1, i, color});
            document->linkedCells[static_cast<size_t>(i)] = cell;
            document->bounds.include(xyz);
            if (progress) {
                const qint64 permille = (i + 1) * 1000 / count;
                if (permille != lastPermille) {
                    lastPermille = permille;
                    progress({QStringLiteral("生成 Job 预览"), i + 1, count});
                }
            }
        }
        if (progress) progress({QStringLiteral("建立 Job 预览空间索引"), 0, count});
        if (!JobSpatialIndex::prepare(document.get(), token)) {
            if (!token->isCancelled() && error)
                *error = QStringLiteral("无法建立 Job 预览空间索引。");
            return {};
        }
        document->sceneBounds = document->bounds;
        if (progress) progress({QStringLiteral("建立 Job 预览空间索引"), count, count});
        return document;
    } catch (const std::bad_alloc&) {
        if (error) *error = QStringLiteral("Job 预览点数过多，内存不足。");
        return {};
    }
}
}

JobDocumentPtr JobPreviewBuilder::build(const ImageMetadata& metadata,
                                        const SegmentationResult& segmentation,
                                        TraversalMode traversalMode,
                                        const PrintExportOptions& options,
                                        const CancellationTokenPtr& token,
                                        const Progress& progress, QString* error,
                                        const ElementGroupingResult* grouping) {
    if (options.canvasX <= 0 || options.canvasY <= 0) {
        if (error) *error = QStringLiteral("无法从当前分割参数生成 Job 预览。");
        return {};
    }
    return buildDocument(metadata, segmentation, traversalMode, token, progress, error,
        [&](const QPoint& cell) {
            return SplitExporter::canvasPosition(metadata, segmentation, cell, options);
        }, grouping);
}

JobDocumentPtr JobPreviewBuilder::build(const ImageMetadata& metadata,
                                        const SegmentationResult& segmentation,
                                        TraversalMode traversalMode,
                                        const JobPlacementOptions& options,
                                        const CancellationTokenPtr& token,
                                        const Progress& progress, QString* error,
                                        const ElementGroupingResult* grouping) {
    if (!std::isfinite(options.pixelScale) || options.pixelScale <= 0.0) {
        if (error) *error = QStringLiteral("无法从当前 pixel2phy 生成 Job 预览。");
        return {};
    }
    return buildDocument(metadata, segmentation, traversalMode, token, progress, error,
        [&](const QPoint& cell) {
            return SplitExporter::physicalPosition(segmentation, cell, options);
        }, grouping);
}

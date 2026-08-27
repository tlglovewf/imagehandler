#pragma once

#include "domain/Models.h"
#include <QPointF>
#include <functional>

struct PrintExportOptions {
    int canvasX = 1;
    int canvasY = 1;
    int repeatTimes = 10;
    bool reverseY = false;
};

class SplitExporter {
public:
    using Progress = std::function<void(const TaskProgress&)>;
    static std::vector<QPoint> traversal(int rows, int columns, TraversalMode mode);
    static qint64 traversalIndex(int rows, int columns, TraversalMode mode,
                                 int row, int column);
    static QPoint nextTraversalCell(int rows, int columns, TraversalMode mode,
                                    int row, int column);
    static QPointF canvasPosition(const ImageMetadata& metadata,
                                  const SegmentationResult& segmentation,
                                  const QPoint& cell,
                                  const PrintExportOptions& options);
    static bool exportTiles(const QString& outputDirectory, const ImageDocumentPtr& document,
                            const SegmentationResult& segmentation, TraversalMode mode,
                            bool overwrite, const CancellationTokenPtr& token,
                            const Progress& progress, QString* error);
    static bool exportPrintPackage(const QString& outputDirectory,
                                   const ImageDocumentPtr& document,
                                   const SegmentationResult& segmentation, TraversalMode mode,
                                   const PrintExportOptions& options, bool overwrite,
                                   const CancellationTokenPtr& token,
                                   const Progress& progress, QString* error);
};

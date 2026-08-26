#pragma once

#include "domain/Models.h"
#include <functional>

class SplitExporter {
public:
    using Progress = std::function<void(const TaskProgress&)>;
    static std::vector<QPoint> traversal(int rows, int columns, TraversalMode mode);
    static qint64 traversalIndex(int rows, int columns, TraversalMode mode,
                                 int row, int column);
    static QPoint nextTraversalCell(int rows, int columns, TraversalMode mode,
                                    int row, int column);
    static bool exportTiles(const QString& outputDirectory, const ImageDocumentPtr& document,
                            const SegmentationResult& segmentation, TraversalMode mode,
                            bool overwrite, const CancellationTokenPtr& token,
                            const Progress& progress, QString* error);
};

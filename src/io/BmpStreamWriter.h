#pragma once

#include "domain/Models.h"
#include <functional>

class BmpStreamWriter {
public:
    using RowProgress = std::function<bool(int completed, int total)>;
    static bool write(const QString& path, const ImageDocumentPtr& document,
                      int x, int y, int width, int height,
                      const SegmentationSettings& settings,
                      const RowProgress& progress, QString* error);
};


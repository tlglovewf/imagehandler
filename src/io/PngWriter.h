#pragma once

#include "domain/Models.h"

#include <functional>

class PngWriter {
public:
    using Progress = std::function<bool(int completed, int total)>;

    static bool write(const QString& path, const ImageDocumentPtr& document,
                      int x, int y, int width, int height,
                      const CancellationTokenPtr& token,
                      const Progress& progress, QString* error);
};

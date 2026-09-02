#pragma once

#include "domain/Models.h"
#include <functional>

class MergedImageExporter {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    static bool writeGray8Bmp(const QString& path, const ImageDocumentPtr& document,
                              const CancellationTokenPtr& token, const Progress& progress,
                              QString* error);
    static bool writePng(const QString& path, const ImageDocumentPtr& document,
                         const CancellationTokenPtr& token, const Progress& progress,
                         QString* error);
};

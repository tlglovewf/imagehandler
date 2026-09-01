#pragma once

#include "domain/MaskSliceModels.h"

#include <functional>

class MaskSliceExporter {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    [[nodiscard]] static quint64 bmpFileSize(int width, int height);
    [[nodiscard]] static bool exportAll(
        const QString& outputDirectory, const MaskSliceRequest& request, int layerCount,
        bool overwrite, const CancellationTokenPtr& token, const Progress& progress,
        QString* error);
};

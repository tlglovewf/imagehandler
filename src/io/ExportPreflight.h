#pragma once

#include "io/ExportTypes.h"

#include <functional>

class ExportPreflight {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    static ExportPreflightResult inspect(const ExportRequest& request,
                                         const CancellationTokenPtr& token = {},
                                         const Progress& progress = {});
    static ExportSizeEstimate estimateOutput(const ExportRequest& request);
    static qint64 bmpFileSize(PixelKind kind, qint64 width, qint64 height);
    static qint64 estimateBmpBytes(const ExportRequest& request, QString* error = nullptr);
    static bool hasSufficientSpace(qint64 requiredBytes, qint64 availableBytes,
                                   bool spaceKnown);
    static QString targetFingerprint(const ExportRequest& request, qint64* conflictCount,
                                     QStringList* conflictSamples, QString* error,
                                     const CancellationTokenPtr& token = {},
                                     const Progress& progress = {});
};

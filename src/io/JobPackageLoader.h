#pragma once

#include "domain/JobModels.h"
#include "domain/MaskSliceModels.h"
#include "domain/Models.h"

#include <functional>

class JobPackageLoader final {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    static JobDocumentPtr load(const QString& path, const CancellationTokenPtr& token,
                               const Progress& progress, QString* error);
    struct StackLoadResult {
        quint64 requestGeneration = 0;
        qint64 placementId = -1;
        QString modelPath;
        QVector<QImage> thumbnails;
        MaskStackPreviewDataPtr preview;
    };

    static StackLoadResult loadStack(const JobDocumentPtr& document, qint64 placementId,
                                     quint64 requestGeneration,
                                     const CancellationTokenPtr& token,
                                     const Progress& progress, QString* error);
};

Q_DECLARE_METATYPE(JobPackageLoader::StackLoadResult)

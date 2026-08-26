#pragma once

#include "domain/Models.h"
#include <functional>

enum class MosaicBuildStatus { Success, MissingTiles };

struct MosaicBuildOptions {
    bool allowMissingTiles = false;
};

struct MosaicBuildResult {
    MosaicBuildStatus status = MosaicBuildStatus::Success;
    ImageDocumentPtr document;
    qint64 missingCount = 0;
    QStringList missingCoordinateSamples;
    int rows = 0;
    int columns = 0;
    int sourceCount = 0;
};

class MosaicBuilder {
public:
    using Progress = std::function<void(const TaskProgress&)>;
    static bool parseName(const QString& path, MergeTile* tile);
    static MosaicBuildResult build(const QStringList& paths, quint64 generation,
                                   const MosaicBuildOptions& options,
                                   const CancellationTokenPtr& token, const Progress& progress,
                                   QString* error);
};

Q_DECLARE_METATYPE(MosaicBuildResult)

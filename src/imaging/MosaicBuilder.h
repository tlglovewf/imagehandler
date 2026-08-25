#pragma once

#include "domain/Models.h"
#include <functional>

class MosaicBuilder {
public:
    using Progress = std::function<void(const TaskProgress&)>;
    static bool parseName(const QString& path, MergeTile* tile);
    static ImageDocumentPtr build(const QStringList& paths, quint64 generation,
                                  const CancellationTokenPtr& token, const Progress& progress,
                                  QString* error);
};


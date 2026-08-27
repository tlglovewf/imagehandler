#pragma once

#include "domain/JobModels.h"
#include "domain/Models.h"

#include <functional>

class JobFileLoader {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    static JobDocumentPtr load(const QString& path, const CancellationTokenPtr& token,
                               const Progress& progress, QString* error);
};

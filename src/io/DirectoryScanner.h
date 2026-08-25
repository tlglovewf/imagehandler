#pragma once

#include "domain/Models.h"
#include <functional>

class DirectoryScanner {
public:
    using Progress = std::function<void(const TaskProgress&)>;
    static QStringList scan(const QString& directory, const CancellationTokenPtr& token,
                            const Progress& progress);
    static bool naturalLessThan(const QString& left, const QString& right);
};


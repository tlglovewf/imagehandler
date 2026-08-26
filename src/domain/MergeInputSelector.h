#pragma once

#include <QStringList>

class MergeInputSelector {
public:
    static QStringList resolve(const QStringList& directoryPaths,
                               const QStringList& selectedPaths);
};

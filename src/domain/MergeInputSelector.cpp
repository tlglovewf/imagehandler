#include "domain/MergeInputSelector.h"

#include <QFileInfo>

QStringList MergeInputSelector::resolve(const QStringList& directoryPaths,
                                        const QStringList& selectedPaths) {
    if (selectedPaths.size() >= 2) return selectedPaths;

    QStringList result;
    for (const auto& path : directoryPaths) {
        if (QFileInfo(path).suffix().compare(QStringLiteral("bmp"), Qt::CaseInsensitive) == 0)
            result << path;
    }
    return result;
}

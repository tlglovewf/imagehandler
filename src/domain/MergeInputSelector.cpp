#include "domain/MergeInputSelector.h"

#include <QFileInfo>
#include <QSet>

namespace {
bool isMergeImage(const QString& path) {
    static const QSet<QString> extensions{QStringLiteral("bmp"), QStringLiteral("png"),
                                          QStringLiteral("tif"), QStringLiteral("tiff")};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}
}

QStringList MergeInputSelector::resolve(const QStringList& directoryPaths,
                                        const QStringList& selectedPaths) {
    if (selectedPaths.size() >= 2) return selectedPaths;

    QStringList result;
    for (const auto& path : directoryPaths) {
        if (isMergeImage(path)) result << path;
    }
    return result;
}

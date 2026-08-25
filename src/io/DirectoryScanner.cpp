#include "io/DirectoryScanner.h"

#include <QCollator>
#include <QDir>
#include <QFileInfoList>
#include <algorithm>

bool DirectoryScanner::naturalLessThan(const QString& left, const QString& right) {
    thread_local QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator.compare(left, right) < 0;
}

QStringList DirectoryScanner::scan(const QString& directory, const CancellationTokenPtr& token,
                                   const Progress& progress) {
    QDir dir(directory);
    const QFileInfoList all = dir.entryInfoList(QDir::Files | QDir::Readable | QDir::NoSymLinks,
                                                QDir::NoSort);
    QStringList result;
    result.reserve(all.size());
    const QStringList extensions{"bmp", "png", "jpg", "jpeg", "tif", "tiff"};
    for (qsizetype i = 0; i < all.size(); ++i) {
        if (token && token->isCancelled()) return {};
        if (extensions.contains(all[i].suffix(), Qt::CaseInsensitive)) result.push_back(all[i].absoluteFilePath());
        if (progress) progress({QStringLiteral("扫描目录"), i + 1, all.size()});
    }
    std::sort(result.begin(), result.end(), [](const QString& a, const QString& b) {
        return naturalLessThan(QFileInfo(a).fileName(), QFileInfo(b).fileName());
    });
    return result;
}


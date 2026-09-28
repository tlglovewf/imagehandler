#include "io/ExportPreflight.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <algorithm>
#include <limits>

namespace {
constexpr qint64 kReportEstimate = 8 * 1024;
constexpr qint64 kMinimumSafetyMargin = 16 * 1024 * 1024;

bool isPresent(const QFileInfo& info) {
    return info.exists() || info.isSymLink() || info.isJunction();
}

bool checkedAdd(qint64 left, qint64 right, qint64* result) {
    if (left < 0 || right < 0 || left > std::numeric_limits<qint64>::max() - right) return false;
    *result = left + right;
    return true;
}

bool checkedMultiply(qint64 left, qint64 right, qint64* result) {
    if (left < 0 || right < 0 || (left != 0 && right > std::numeric_limits<qint64>::max() / left))
        return false;
    *result = left * right;
    return true;
}

QString entryRecord(const QFileInfo& info) {
    const QString type = info.isSymLink() ? QStringLiteral("link")
        : info.isJunction() ? QStringLiteral("junction")
        : info.isDir() ? QStringLiteral("directory")
        : info.isFile() ? QStringLiteral("file") : QStringLiteral("other");
    return QStringLiteral("%1|%2|%3|%4")
        .arg(info.fileName(), type)
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());
}

bool safeTarget(const QFileInfo& info, bool expectDirectory) {
    if (info.isSymLink() || info.isJunction()) return false;
    return expectDirectory ? info.isDir() : info.isFile();
}
}

qint64 ExportPreflight::bmpFileSize(PixelKind kind, qint64 width, qint64 height) {
    if (width <= 0 || height <= 0) return -1;
    const qint64 bits = kind == PixelKind::Binary1 ? 1 : kind == PixelKind::Gray16 ? 16 : 8;
    if (width > (std::numeric_limits<qint64>::max() - 31) / bits) return -1;
    const qint64 stride = ((width * bits + 31) / 32) * 4;
    const qint64 palette = bits == 1 ? 8 : bits == 8 ? 1024 : 0;
    qint64 pixels = 0;
    if (!checkedMultiply(stride, height, &pixels)) return -1;
    qint64 size = 0;
    if (!checkedAdd(14 + 40 + palette, pixels, &size) || size > std::numeric_limits<quint32>::max())
        return -1;
    return size;
}

qint64 ExportPreflight::estimateBmpBytes(const ExportRequest& request, QString* error) {
    if (!request.document || request.segmentation.rows <= 0 || request.segmentation.columns <= 0 ||
        request.segmentation.settings.cellWidth <= 0 ||
        request.segmentation.settings.cellHeight <= 0) {
        if (error) *error = QStringLiteral("导出文档或分割参数无效。");
        return -1;
    }
    const auto& metadata = request.document->metadata;
    const qint64 columns = request.segmentation.columns;
    const qint64 rows = request.segmentation.rows;
    const qint64 lastWidth = metadata.width - (columns - 1) *
                             qint64(request.segmentation.settings.cellWidth);
    const qint64 lastHeight = metadata.height - (rows - 1) *
                              qint64(request.segmentation.settings.cellHeight);
    if (lastWidth <= 0 || lastWidth > request.segmentation.settings.cellWidth ||
        lastHeight <= 0 || lastHeight > request.segmentation.settings.cellHeight) {
        if (error) *error = QStringLiteral("分割网格与当前图像尺寸不匹配。");
        return -1;
    }

    struct Group { qint64 width; qint64 height; qint64 count; };
    const Group groups[] = {
        {request.segmentation.settings.cellWidth, request.segmentation.settings.cellHeight,
         std::max<qint64>(0, columns - 1) * std::max<qint64>(0, rows - 1)},
        {lastWidth, request.segmentation.settings.cellHeight, std::max<qint64>(0, rows - 1)},
        {request.segmentation.settings.cellWidth, lastHeight, std::max<qint64>(0, columns - 1)},
        {lastWidth, lastHeight, 1}
    };
    qint64 total = 0;
    for (const Group& group : groups) {
        if (group.count == 0) continue;
        const qint64 size = bmpFileSize(metadata.kind, group.width, group.height);
        qint64 subtotal = 0;
        if (size < 0 || !checkedMultiply(size, group.count, &subtotal) ||
            !checkedAdd(total, subtotal, &total)) {
            if (error) *error = QStringLiteral("BMP 输出尺寸超过程序可表示范围。");
            return -1;
        }
    }
    if (request.mode == ExportMode::PrintPackage) {
        qint64 repeated = 0;
        if (request.printOptions.repeatTimes <= 0 ||
            !checkedMultiply(total, request.printOptions.repeatTimes, &repeated)) {
            if (error) *error = QStringLiteral("打印文件数量或尺寸超过程序可表示范围。");
            return -1;
        }
        total = repeated;
    }
    return total;
}

ExportSizeEstimate ExportPreflight::estimateOutput(const ExportRequest& request) {
    ExportSizeEstimate estimate;
    if (!request.document) {
        estimate.error = QStringLiteral("没有可估算的图像。");
        return estimate;
    }
    const qint64 cells = qint64(request.segmentation.rows) * request.segmentation.columns;
    if (cells <= 0) {
        estimate.error = QStringLiteral("分割数量无效。");
        return estimate;
    }
    if (request.mode == ExportMode::PrintPackage &&
        (request.printOptions.repeatTimes <= 0 ||
         cells > std::numeric_limits<qint64>::max() / request.printOptions.repeatTimes)) {
        estimate.error = QStringLiteral("打印文件数量超出程序可表示范围。");
        return estimate;
    }
    estimate.bmpFileCount = request.mode == ExportMode::PrintPackage
        ? cells * request.printOptions.repeatTimes : cells;
    estimate.estimatedBmpBytes = estimateBmpBytes(request, &estimate.error);
    if (estimate.estimatedBmpBytes < 0) return estimate;

    qint64 auxiliary = request.writeReport ? kReportEstimate : 0;
    if (request.mode == ExportMode::PrintPackage) {
        qint64 jobEstimate = 0;
        if (!checkedMultiply(cells, 220, &jobEstimate) ||
            !checkedAdd(auxiliary, jobEstimate, &auxiliary)) {
            estimate.error = QStringLiteral("Job 报告尺寸超过程序可表示范围。");
            return estimate;
        }
    }
    if (!checkedAdd(estimate.estimatedBmpBytes, auxiliary,
                    &estimate.estimatedTotalBytes)) {
        estimate.error = QStringLiteral("预计输出尺寸超过程序可表示范围。");
        return estimate;
    }
    const qint64 margin = std::max(kMinimumSafetyMargin,
                                   estimate.estimatedTotalBytes / 20);
    if (!checkedAdd(estimate.estimatedTotalBytes, margin, &estimate.requiredBytes)) {
        estimate.error = QStringLiteral("预计所需空间超过程序可表示范围。");
        return estimate;
    }
    estimate.valid = true;
    return estimate;
}

bool ExportPreflight::hasSufficientSpace(qint64 requiredBytes, qint64 availableBytes,
                                         bool spaceKnown) {
    return !spaceKnown || (requiredBytes >= 0 && availableBytes >= requiredBytes);
}

QString ExportPreflight::targetFingerprint(const ExportRequest& request, qint64* conflictCount,
                                           QStringList* conflictSamples, QString* error,
                                           const CancellationTokenPtr& token,
                                           const Progress& progress) {
    if (conflictCount) *conflictCount = 0;
    if (conflictSamples) conflictSamples->clear();
    QDir output(request.outputDirectory);
    if (!output.exists()) {
        if (error) *error = QStringLiteral("输出目录不存在。");
        return {};
    }

    const qint64 cellCount = qint64(request.segmentation.rows) * request.segmentation.columns;
    const QRegularExpression tileName(QStringLiteral("^([1-9][0-9]*)\\.bmp$"),
                                      QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression numericName(QStringLiteral("^[0-9]+$"));
    QStringList records;
    const QFileInfoList entries = output.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::Name | QDir::IgnoreCase);
    qint64 completed = 0;
    for (const QFileInfo& info : entries) {
        if (token && token->isCancelled()) return {};
        const QString name = info.fileName();
        bool managed = false;
        bool expectDirectory = false;
        if (request.mode == ExportMode::SplitTiles) {
            const auto match = tileName.match(name);
            bool ok = false;
            const qint64 index = match.hasMatch() ? match.captured(1).toLongLong(&ok) : -1;
            managed = ok && index >= 1 && index <= cellCount;
        } else if (numericName.match(name).hasMatch()) {
            managed = true;
            expectDirectory = true;
        } else if (name.compare(QStringLiteral("job.json"), Qt::CaseInsensitive) == 0) {
            managed = true;
        }
        if (name.compare(QStringLiteral("export-report.json"), Qt::CaseInsensitive) == 0) {
            managed = true;
            expectDirectory = false;
        }
        if (managed) {
            if (!safeTarget(info, expectDirectory)) {
                if (error) *error = QStringLiteral("目标路径类型不安全，无法替换：%1").arg(name);
                return {};
            }
            records << entryRecord(info);
            if (conflictCount) ++*conflictCount;
            if (conflictSamples && conflictSamples->size() < 10) *conflictSamples << name;
        }
        ++completed;
        if (progress && (completed == entries.size() || completed % 128 == 0))
            progress({QStringLiteral("检查目标目录"), completed, entries.size()});
    }
    std::sort(records.begin(), records.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseSensitive) < 0;
    });
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString& record : records) {
        hash.addData(record.toUtf8());
        hash.addData("\n", 1);
    }
    return QString::fromLatin1(hash.result().toHex());
}

ExportPreflightResult ExportPreflight::inspect(const ExportRequest& request,
                                               const CancellationTokenPtr& token,
                                               const Progress& progress) {
    ExportPreflightResult result;
    if (!request.document || !request.document->image) {
        result.blockingError = QStringLiteral("没有可导出的图像。");
        return result;
    }
    const ExportSizeEstimate estimate = estimateOutput(request);
    result.bmpFileCount = estimate.bmpFileCount;
    result.estimatedBmpBytes = estimate.estimatedBmpBytes;
    result.estimatedTotalBytes = estimate.estimatedTotalBytes;
    result.requiredBytes = estimate.requiredBytes;
    result.blockingError = estimate.error;
    if (!estimate.valid) return result;

    const qint64 cells = qint64(request.segmentation.rows) * request.segmentation.columns;
    result.firstOutput = request.mode == ExportMode::PrintPackage
        ? QStringLiteral("0/1.bmp") : QStringLiteral("1.bmp");
    result.lastOutput = request.mode == ExportMode::PrintPackage
        ? QStringLiteral("%1/%2.bmp").arg(cells - 1).arg(request.printOptions.repeatTimes)
        : QStringLiteral("%1.bmp").arg(cells);

    result.targetFingerprint = targetFingerprint(request, &result.conflictCount,
        &result.conflictSamples, &result.blockingError, token, progress);
    if (!result.blockingError.isEmpty() || (token && token->isCancelled())) return result;

    QStorageInfo storage(request.outputDirectory);
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0) {
        result.spaceKnown = true;
        result.availableBytes = storage.bytesAvailable();
        if (!hasSufficientSpace(result.requiredBytes, result.availableBytes, true)) {
            result.blockingError = QStringLiteral("输出磁盘空间不足。");
            return result;
        }
    } else {
        result.warnings << QStringLiteral("无法获取输出目录的可用空间，导出仍可能因空间不足失败。");
    }
    result.valid = true;
    return result;
}

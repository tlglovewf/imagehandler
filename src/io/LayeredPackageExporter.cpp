#include "io/LayeredPackageExporter.h"

#include "domain/StorageSizeFormatter.h"
#include "imaging/LayeredMaskProcessor.h"
#include "io/SplitExporter.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr auto kReportName = "export-report.json";
constexpr qint64 kReportEstimate = 8 * 1024;
constexpr qint64 kMinimumSafetyMargin = 16 * 1024 * 1024;

bool checkedAdd(qint64 left, qint64 right, qint64* result) {
    if (left < 0 || right < 0 || left > std::numeric_limits<qint64>::max() - right)
        return false;
    *result = left + right;
    return true;
}

bool checkedMultiply(qint64 left, qint64 right, qint64* result) {
    if (left < 0 || right < 0 ||
        (left != 0 && right > std::numeric_limits<qint64>::max() / left)) return false;
    *result = left * right;
    return true;
}

bool hasGrouping(const LayeredPackageRequest& request) {
    return request.grouping.matches(request.document, request.segmentation) &&
           !request.grouping.groups.isEmpty();
}

qint64 modelCount(const LayeredPackageRequest& request,
                  const std::vector<QPoint>& order) {
    return hasGrouping(request) ? request.grouping.groups.size()
                                : static_cast<qint64>(order.size());
}

int modelIdFor(const LayeredPackageRequest& request, const QPoint& cell,
               qsizetype traversalIndex) {
    return hasGrouping(request) ? request.grouping.groupIdAt(cell.y(), cell.x())
                                : static_cast<int>(traversalIndex);
}

QVector<QPoint> representativeCells(const LayeredPackageRequest& request,
                                    const std::vector<QPoint>& order) {
    QVector<QPoint> result;
    if (hasGrouping(request)) {
        result.reserve(request.grouping.groups.size());
        for (const ElementGroup& group : request.grouping.groups)
            result.push_back(group.representativeCell);
    } else {
        result.reserve(static_cast<qsizetype>(order.size()));
        for (const QPoint& cell : order) result.push_back(cell);
    }
    return result;
}

QString jsonNumber(double value) {
    if (value == 0.0) return QStringLiteral("0.0");
    QString number = QString::number(value, 'g', QLocale::FloatingPointShortest);
    if (!number.contains(QLatin1Char('.'))) {
        const qsizetype exponent = std::max(number.indexOf(QLatin1Char('e')),
                                             number.indexOf(QLatin1Char('E')));
        if (exponent >= 0) number.insert(exponent, QStringLiteral(".0"));
        else number += QStringLiteral(".0");
    }
    return number;
}

bool writeAll(QFile& file, const QByteArray& data, QString* error) {
    qsizetype offset = 0;
    while (offset < data.size()) {
        const qint64 written = file.write(data.constData() + offset, data.size() - offset);
        if (written <= 0) {
            if (error) *error = QStringLiteral("无法写入 job.json：%1").arg(file.errorString());
            return false;
        }
        offset += static_cast<qsizetype>(written);
    }
    return true;
}

bool writeJobJson(const QString& path, const LayeredPackageRequest& request,
                  const std::vector<QPoint>& order, const CancellationTokenPtr& token,
                  QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法创建 job.json：%1").arg(file.errorString());
        return false;
    }
    if (!writeAll(file, QByteArrayLiteral("{\n  \"models\": [\n"), error)) return false;
    const qint64 models = modelCount(request, order);
    for (qint64 i = 0; i < models; ++i) {
        if (token && token->isCancelled()) return false;
        const QByteArray item = QStringLiteral(
            "    {\n      \"label\": \"%1\",\n      \"path\": \"%1\"\n    }%2\n")
            .arg(i).arg(i + 1 < models ? QStringLiteral(",") : QString()).toUtf8();
        if (!writeAll(file, item, error)) return false;
    }
    if (!writeAll(file, QByteArrayLiteral("  ],\n  \"commands\": [\n"), error)) return false;
    const JobPlacementOptions placement{request.options.pixelScale, request.options.reverseY};
    for (qsizetype i = 0; i < static_cast<qsizetype>(order.size()); ++i) {
        if (token && token->isCancelled()) return false;
        const QPointF position = SplitExporter::physicalPosition(
            request.segmentation, order[static_cast<size_t>(i)], placement);
        const int modelId = modelIdFor(request, order[static_cast<size_t>(i)], i);
        const QByteArray item = QStringLiteral(
            "    {\n      \"command\": \"relmove\",\n      \"by\": [%1, %2, 0.0]\n    },\n"
            "    {\n      \"command\": \"place\",\n      \"id\": %3\n    }%4\n")
            .arg(jsonNumber(position.x()), jsonNumber(position.y())).arg(modelId)
            .arg(i + 1 < static_cast<qsizetype>(order.size()) ? QStringLiteral(",")
                                                               : QString()).toUtf8();
        if (!writeAll(file, item, error)) return false;
    }
    if (!writeAll(file, QByteArrayLiteral("  ]\n}\n"), error) || !file.flush()) {
        if (error && error->isEmpty()) *error = QStringLiteral("无法保存 job.json：%1")
                                                 .arg(file.errorString());
        return false;
    }
    return true;
}

bool writeReport(const QString& path, const LayeredPackageRequest& request, int layers,
                 qint64 estimatedBytes, qint64 overwritten, QString* error) {
    const auto strategy = request.options.layerCountStrategy == LayerCountStrategy::RoundUp
        ? QStringLiteral("round_up") : QStringLiteral("require_exact");
    const auto sampling = request.options.transformSampling == BinaryTransformSampling::Nearest
        ? QStringLiteral("nearest") : QStringLiteral("smooth_threshold");
    const qint64 cells = qint64(request.segmentation.rows) * request.segmentation.columns;
    const qint64 groups = hasGrouping(request) ? request.grouping.groups.size() : cells;
    QJsonObject root{
        {QStringLiteral("schemaVersion"), 3},
        {QStringLiteral("application"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("ImageHandler")},
            {QStringLiteral("version"), QCoreApplication::applicationVersion().isEmpty()
                ? QStringLiteral(IMAGE_HANDLER_VERSION) : QCoreApplication::applicationVersion()}}},
        {QStringLiteral("createdAtUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("mode"), QStringLiteral("layered_package")},
        {QStringLiteral("traversal"), request.traversalMode == TraversalMode::SnakeZ
            ? QStringLiteral("snake_z") : QStringLiteral("clockwise_spiral")},
        {QStringLiteral("mapping"), QJsonObject{
            {QStringLiteral("pixel2phy"), request.options.pixelScale},
            {QStringLiteral("maximumHeight"), request.options.maximumHeight},
            {QStringLiteral("invertHeight"), request.options.invertHeight},
            {QStringLiteral("reverseY"), request.options.reverseY}}},
        {QStringLiteral("layers"), QJsonObject{
            {QStringLiteral("layerHeight"), request.options.layerHeight},
            {QStringLiteral("count"), layers},
            {QStringLiteral("strategy"), strategy},
            {QStringLiteral("invertMask"), request.options.invertMask}}},
        {QStringLiteral("output"), QJsonObject{
            {QStringLiteral("directoryCount"), groups},
            {QStringLiteral("bmpCount"), groups * layers},
            {QStringLiteral("placementCount"), cells},
            {QStringLiteral("uniquePatternCount"), groups},
            {QStringLiteral("reusedPlacementCount"), cells - groups},
            {QStringLiteral("groupingStrategy"), hasGrouping(request)
                ? request.grouping.strategyId : QStringLiteral("none")},
            {QStringLiteral("standardWidth"), request.options.outputWidth},
            {QStringLiteral("standardHeight"), request.options.outputHeight},
            {QStringLiteral("residualAlignment"), QStringLiteral("top_left")},
            {QStringLiteral("attachToCanvas"), request.options.attachToCanvas},
            {QStringLiteral("canvasWidth"), request.options.canvasWidth},
            {QStringLiteral("canvasHeight"), request.options.canvasHeight},
            {QStringLiteral("rotationDegrees"), request.options.rotationDegrees},
            {QStringLiteral("scaleX"), request.options.scaleX},
            {QStringLiteral("scaleY"), request.options.scaleY},
            {QStringLiteral("transformSampling"), sampling},
            {QStringLiteral("bitsPerPixel"), 1},
            {QStringLiteral("namingRule"), QStringLiteral("{placeId}/{layer}.bmp")},
            {QStringLiteral("estimatedBmpBytes"), estimatedBytes},
            {QStringLiteral("overwrittenTargetCount"), overwritten}}}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法创建导出报告：%1").arg(file.errorString());
        return false;
    }
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("无法保存导出报告：%1").arg(file.errorString());
        return false;
    }
    return true;
}

bool safeTarget(const QFileInfo& info, bool directory) {
    if (info.isSymLink() || info.isJunction()) return false;
    return directory ? info.isDir() : info.isFile();
}

QString entryRecord(const QFileInfo& info) {
    const QString type = info.isSymLink() ? QStringLiteral("link")
        : info.isJunction() ? QStringLiteral("junction")
        : info.isDir() ? QStringLiteral("directory")
        : info.isFile() ? QStringLiteral("file") : QStringLiteral("other");
    return QStringLiteral("%1|%2|%3|%4").arg(info.fileName(), type)
        .arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
}

QString targetFingerprint(const LayeredPackageRequest& request, qint64* count,
                          QStringList* samples, QString* error,
                          const CancellationTokenPtr& token,
                          const LayeredPackageExporter::Progress& progress) {
    if (count) *count = 0;
    if (samples) samples->clear();
    QDir output(request.outputDirectory);
    if (!output.exists()) {
        if (error) *error = QStringLiteral("输出目录不存在。");
        return {};
    }
    const QRegularExpression numeric(QStringLiteral("^[0-9]+$"));
    const QFileInfoList entries = output.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::Name | QDir::IgnoreCase);
    QStringList records;
    qint64 completed = 0;
    for (const QFileInfo& info : entries) {
        if (token && token->isCancelled()) return {};
        const QString name = info.fileName();
        const bool isDirectory = numeric.match(name).hasMatch();
        const bool isFile = name.compare(QStringLiteral("job.json"), Qt::CaseInsensitive) == 0 ||
            (request.writeReport && name.compare(QString::fromLatin1(kReportName),
                                                 Qt::CaseInsensitive) == 0);
        if (isDirectory || isFile) {
            if (!safeTarget(info, isDirectory)) {
                if (error) *error = QStringLiteral("目标路径类型不安全，无法替换：%1").arg(name);
                return {};
            }
            records << entryRecord(info);
            if (count) ++*count;
            if (samples && samples->size() < 10) *samples << name;
        }
        ++completed;
        if (progress && (completed == entries.size() || completed % 128 == 0))
            progress({QStringLiteral("检查目标目录"), completed, entries.size()});
    }
    std::sort(records.begin(), records.end());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString& record : records) {
        hash.addData(record.toUtf8());
        hash.addData("\n", 1);
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool removeEntry(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink() && !info.isJunction()) return true;
    if (info.isFile() || info.isSymLink() || info.isJunction()) return QFile::remove(path);
    return info.isDir() && QDir(path).removeRecursively();
}
}

int LayeredPackageExporter::layerCount(const LayeredPackageOptions& options, QString* error) {
    if (error) error->clear();
    if (!std::isfinite(options.maximumHeight) || options.maximumHeight <= 0.0 ||
        !std::isfinite(options.layerHeight) || options.layerHeight <= 0.0) {
        if (error) *error = QStringLiteral("最大高度和层切高必须是有效的正数。");
        return 0;
    }
    const double quotient = options.maximumHeight / options.layerHeight;
    double layers = 0.0;
    if (options.layerCountStrategy == LayerCountStrategy::RequireExact) {
        const double nearest = std::round(quotient);
        const double tolerance = 1e-9 * std::max(1.0, std::abs(quotient));
        if (std::abs(quotient - nearest) > tolerance) {
            if (error) *error = QStringLiteral("最大高度必须能被层切高整除。");
            return 0;
        }
        layers = nearest;
    } else {
        layers = std::ceil(quotient - 1e-12);
    }
    if (!std::isfinite(layers) || layers < 1.0 || layers > kMaximumMaskSliceLayers) {
        if (error) *error = layers > kMaximumMaskSliceLayers
            ? QStringLiteral("切片数量超过 %1 张，请增大层切高。").arg(kMaximumMaskSliceLayers)
            : QStringLiteral("无法计算切片数量。");
        return 0;
    }
    return static_cast<int>(layers);
}

ExportSizeEstimate LayeredPackageExporter::estimateOutput(const LayeredPackageRequest& request) {
    ExportSizeEstimate result;
    const int layers = layerCount(request.options, &result.error);
    if (!request.document || !request.document->image || layers <= 0 ||
        request.segmentation.rows <= 0 || request.segmentation.columns <= 0 ||
        request.segmentation.settings.cellWidth <= 0 ||
        request.segmentation.settings.cellHeight <= 0 || request.options.outputWidth <= 0 ||
        request.options.outputHeight <= 0 || !std::isfinite(request.options.pixelScale) ||
        request.options.pixelScale <= 0.0) {
        if (result.error.isEmpty()) result.error = QStringLiteral("分层导出参数无效。");
        return result;
    }
    const auto order = SplitExporter::traversal(request.segmentation.rows,
                                                 request.segmentation.columns,
                                                 request.traversalMode);
    const QVector<QPoint> exports = representativeCells(request, order);
    qint64 bytes = 0;
    for (const QPoint& cell : exports) {
        const ImageRegion region = segmentationCellRegion(
            request.segmentation, request.document->metadata.width,
            request.document->metadata.height, cell.y(), cell.x());
        if (!region.isValid()) {
            result.error = QStringLiteral("分割网格与当前图像尺寸不匹配。");
            return result;
        }
        const LayeredMaskRequest mask{request.document, region,
            {request.segmentation.settings.cellWidth, request.segmentation.settings.cellHeight},
            request.options, layers};
        QString maskError;
        if (!LayeredMaskProcessor::validate(mask, &maskError)) {
            result.error = maskError;
            return result;
        }
        const quint64 one = LayeredMaskProcessor::bmpFileSize(
            LayeredMaskProcessor::outputSize(request.options));
        qint64 block = 0;
        if (one == 0 || one > quint64(std::numeric_limits<qint64>::max()) ||
            !checkedMultiply(static_cast<qint64>(one), layers, &block) ||
            !checkedAdd(bytes, block, &bytes)) {
            result.error = QStringLiteral("BMP 输出尺寸超过程序可表示范围。");
            return result;
        }
    }
    const qint64 uniquePatterns = exports.size();
    if (!checkedMultiply(uniquePatterns, layers, &result.bmpFileCount)) {
        result.error = QStringLiteral("BMP 文件数量超过程序可表示范围。");
        return result;
    }
    result.estimatedBmpBytes = bytes;
    qint64 jobEstimate = 0;
    qint64 auxiliary = request.writeReport ? kReportEstimate : 0;
    if (!checkedMultiply(static_cast<qint64>(order.size()), 220, &jobEstimate) ||
        !checkedAdd(auxiliary, jobEstimate, &auxiliary) ||
        !checkedAdd(bytes, auxiliary, &result.estimatedTotalBytes)) {
        result.error = QStringLiteral("预计输出尺寸超过程序可表示范围。");
        return result;
    }
    const qint64 margin = std::max(kMinimumSafetyMargin, result.estimatedTotalBytes / 20);
    if (!checkedAdd(result.estimatedTotalBytes, margin, &result.requiredBytes)) {
        result.error = QStringLiteral("预计所需空间超过程序可表示范围。");
        return result;
    }
    result.valid = true;
    return result;
}

ExportPreflightResult LayeredPackageExporter::inspect(
    const LayeredPackageRequest& request, const CancellationTokenPtr& token,
    const Progress& progress) {
    ExportPreflightResult result;
    const ExportSizeEstimate estimate = estimateOutput(request);
    result.bmpFileCount = estimate.bmpFileCount;
    result.estimatedBmpBytes = estimate.estimatedBmpBytes;
    result.estimatedTotalBytes = estimate.estimatedTotalBytes;
    result.requiredBytes = estimate.requiredBytes;
    result.blockingError = estimate.error;
    if (!estimate.valid) return result;
    const int layers = layerCount(request.options);
    const qint64 cells = hasGrouping(request) ? request.grouping.groups.size()
                                              : qint64(request.segmentation.rows) * request.segmentation.columns;
    result.firstOutput = QStringLiteral("0/1.bmp");
    result.lastOutput = QStringLiteral("%1/%2.bmp").arg(cells - 1).arg(layers);
    result.targetFingerprint = targetFingerprint(request, &result.conflictCount,
        &result.conflictSamples, &result.blockingError, token, progress);
    if (!result.blockingError.isEmpty() || (token && token->isCancelled())) return result;
    QStorageInfo storage(request.outputDirectory);
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0) {
        result.spaceKnown = true;
        result.availableBytes = storage.bytesAvailable();
        if (result.availableBytes < result.requiredBytes) {
            result.blockingError = QStringLiteral("输出磁盘空间不足。");
            return result;
        }
    } else {
        result.warnings << QStringLiteral("无法获取输出目录可用空间，导出可能因空间不足失败。");
    }
    result.valid = true;
    return result;
}

bool LayeredPackageExporter::exportPackage(
    const LayeredPackageRequest& request, const ExportExecutionOptions& execution,
    const CancellationTokenPtr& token, const Progress& progress, QString* error) {
    if (error) error->clear();
    if (!token) {
        if (error) *error = QStringLiteral("分层导出缺少取消令牌。");
        return false;
    }
    const ExportSizeEstimate estimate = estimateOutput(request);
    const int layers = layerCount(request.options, error);
    if (!estimate.valid || layers <= 0) {
        if (error && error->isEmpty()) *error = estimate.error;
        return false;
    }
    QDir output(request.outputDirectory);
    if (!output.exists()) {
        if (error) *error = QStringLiteral("输出目录不存在。");
        return false;
    }
    qint64 conflicts = 0;
    QString fingerprintError;
    const QString fingerprint = targetFingerprint(request, &conflicts, nullptr,
        &fingerprintError, token, {});
    if (!fingerprintError.isEmpty()) {
        if (error) *error = fingerprintError;
        return false;
    }
    if (!execution.confirmedTargetFingerprint.isEmpty() &&
        fingerprint != execution.confirmedTargetFingerprint) {
        if (error) *error = QStringLiteral("输出目录内容在预检后发生变化，请重新执行预检。");
        return false;
    }
    if (conflicts > 0 && !execution.overwrite) {
        if (error) *error = QStringLiteral("LAYERED_TARGETS_EXIST:%1").arg(conflicts);
        return false;
    }

    const QString tempName = QStringLiteral(".image_handler_layered_%1")
        .arg(QUuid::createUuid().toString(QUuid::Id128));
    if (!output.mkdir(tempName)) {
        if (error) *error = QStringLiteral("无法创建分层导出临时目录。");
        return false;
    }
    QDir temp(output.filePath(tempName));
    if (!temp.mkdir(QStringLiteral("staged")) || !temp.mkdir(QStringLiteral("backup"))) {
        temp.removeRecursively();
        if (error) *error = QStringLiteral("无法初始化分层导出临时目录。");
        return false;
    }
    QDir staged(temp.filePath(QStringLiteral("staged")));
    QDir backup(temp.filePath(QStringLiteral("backup")));
    const auto cleanup = [&] { return temp.removeRecursively(); };
    const auto order = SplitExporter::traversal(request.segmentation.rows,
                                                 request.segmentation.columns,
                                                 request.traversalMode);
    const QVector<QPoint> exports = representativeCells(request, order);
    const qint64 total = static_cast<qint64>(exports.size()) + 2;
    for (qsizetype index = 0; index < exports.size(); ++index) {
        if (token->isCancelled()) { cleanup(); return false; }
        const QString directoryName = QString::number(index);
        if (!staged.mkdir(directoryName)) {
            cleanup();
            if (error) *error = QStringLiteral("无法创建分块目录：%1").arg(directoryName);
            return false;
        }
        const QPoint cell = exports[index];
        const ImageRegion region = segmentationCellRegion(
            request.segmentation, request.document->metadata.width,
            request.document->metadata.height, cell.y(), cell.x());
        const LayeredMaskRequest slice{request.document, region,
            {request.segmentation.settings.cellWidth, request.segmentation.settings.cellHeight},
            request.options, layers};
        QString sliceError;
        if (!LayeredMaskProcessor::exportAll(staged.filePath(directoryName), slice, token, {},
                                             &sliceError)) {
            cleanup();
            if (!token->isCancelled() && error)
                *error = QStringLiteral("分块 %1 导出失败：%2").arg(index).arg(sliceError);
            return false;
        }
        if (progress) progress({QStringLiteral("导出图元组 %1/%2").arg(index + 1).arg(exports.size()),
                                index + 1, total});
    }
    if (!writeJobJson(staged.filePath(QStringLiteral("job.json")), request, order, token,
                      error)) { cleanup(); return false; }
    if (progress) progress({QStringLiteral("生成 job.json"), qint64(exports.size()) + 1, total});
    if (request.writeReport &&
        !writeReport(staged.filePath(QString::fromLatin1(kReportName)), request, layers,
                     estimate.estimatedBmpBytes, conflicts, error)) {
        cleanup();
        return false;
    }
    if (token->isCancelled()) { cleanup(); return false; }
    const QString currentFingerprint = targetFingerprint(request, nullptr, nullptr,
        &fingerprintError, token, {});
    if (!fingerprintError.isEmpty() || currentFingerprint != fingerprint) {
        cleanup();
        if (error) *error = fingerprintError.isEmpty()
            ? QStringLiteral("输出目录内容在导出期间发生变化，请重试。") : fingerprintError;
        return false;
    }

    QStringList existingTargets;
    const QRegularExpression numeric(QStringLiteral("^[0-9]+$"));
    for (const QFileInfo& info : output.entryInfoList(
             QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (numeric.match(info.fileName()).hasMatch() ||
            info.fileName().compare(QStringLiteral("job.json"), Qt::CaseInsensitive) == 0 ||
            (request.writeReport && info.fileName().compare(QString::fromLatin1(kReportName),
                                                             Qt::CaseInsensitive) == 0))
            existingTargets << info.fileName();
    }
    QStringList backedUp;
    QStringList committed;
    for (const QString& name : existingTargets) {
        if (token->isCancelled() || !QDir().rename(output.filePath(name), backup.filePath(name))) {
            if (error && error->isEmpty()) *error = QStringLiteral("无法备份旧结果：%1").arg(name);
            goto rollback;
        }
        backedUp << name;
    }
    for (qsizetype index = 0; index < exports.size(); ++index) {
        const QString name = QString::number(index);
        if (token->isCancelled() || !QDir().rename(staged.filePath(name), output.filePath(name))) {
            if (error && error->isEmpty()) *error = QStringLiteral("无法提交分块目录：%1").arg(name);
            goto rollback;
        }
        committed << name;
    }
    for (const QString& name : QStringList{QStringLiteral("job.json"),
             request.writeReport ? QString::fromLatin1(kReportName) : QString{}}) {
        if (name.isEmpty()) continue;
        if (!QDir().rename(staged.filePath(name), output.filePath(name))) {
            if (error) *error = QStringLiteral("无法提交 %1。").arg(name);
            goto rollback;
        }
        committed << name;
    }
    if (progress) progress({QStringLiteral("提交分层导出结果"), total, total});
    cleanup();
    return true;

rollback:
    {
        QStringList rollbackErrors;
        for (auto it = committed.crbegin(); it != committed.crend(); ++it)
            if (!removeEntry(output.filePath(*it))) rollbackErrors << *it;
        for (auto it = backedUp.crbegin(); it != backedUp.crend(); ++it)
            if (QFileInfo::exists(output.filePath(*it)) ||
                !QDir().rename(backup.filePath(*it), output.filePath(*it)))
                rollbackErrors << *it;
        if (rollbackErrors.isEmpty()) cleanup();
        else if (error) *error += QStringLiteral("\n回滚未完全成功，备份保留在：%1")
                                      .arg(temp.absolutePath());
    }
    return false;
}

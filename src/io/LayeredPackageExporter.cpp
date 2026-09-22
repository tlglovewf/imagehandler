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
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QThread>
#include <QThreadPool>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace {
constexpr auto kReportName = "export-report.json";
constexpr auto kParameterName = "param.txt";
constexpr qint64 kReportEstimate = 8 * 1024;
constexpr qint64 kMinimumSafetyMargin = 16 * 1024 * 1024;
constexpr quint64 kExportWorkerMemoryBudget = 256ULL * 1024ULL * 1024ULL;
constexpr int kMaximumExportWorkers = 2;

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

int exportWorkerCount(const LayeredPackageRequest& request, qsizetype groupCount) {
    if (groupCount <= 1) return 1;
    const quint64 sourcePixels = quint64(request.options.outputWidth) *
                                 quint64(request.options.outputHeight);
    const QSize finalSize = LayeredMaskProcessor::outputSize(request.options);
    const quint64 finalPixels = quint64(finalSize.width()) * quint64(finalSize.height());
    const quint64 rowBytes = ((quint64(finalSize.width()) + 31) / 32) * 4;
    const quint64 perWorker = std::max<quint64>(1,
        sourcePixels * sizeof(quint16) + finalPixels * sizeof(quint16) + rowBytes * 2);
    const int memoryLimit = static_cast<int>(std::max<quint64>(
        1, std::min<quint64>(kMaximumExportWorkers,
                             kExportWorkerMemoryBudget / perWorker)));
    const int cpuLimit = std::max(1, QThread::idealThreadCount() - 1);
    return std::max(1, std::min({kMaximumExportWorkers, memoryLimit, cpuLimit,
                                static_cast<int>(groupCount)}));
}

bool hasGrouping(const LayeredPackageRequest& request) {
    return request.grouping.matches(request.document, request.segmentation) &&
           !request.grouping.groups.isEmpty();
}

struct PackageLayout {
    std::vector<QPoint> placements;
    QVector<QPoint> models;
    QVector<int> modelIds;
};

PackageLayout packageLayout(const LayeredPackageRequest& request) {
    PackageLayout result;
    const auto fullOrder = SplitExporter::traversal(
        request.segmentation.rows, request.segmentation.columns, request.traversalMode);
    const bool selectedOnly = request.selectedCells.size() > 1;
    if (!selectedOnly) {
        result.placements = fullOrder;
    } else {
        const qsizetype cellCount = qsizetype(request.segmentation.rows) *
                                    request.segmentation.columns;
        QVector<bool> selected(cellCount, false);
        for (const QPoint& cell : request.selectedCells) {
            if (cell.x() < 0 || cell.y() < 0 ||
                cell.x() >= request.segmentation.columns ||
                cell.y() >= request.segmentation.rows)
                continue;
            selected[qsizetype(cell.y()) * request.segmentation.columns + cell.x()] = true;
        }
        result.placements.reserve(static_cast<size_t>(request.selectedCells.size()));
        for (const QPoint& cell : fullOrder) {
            if (selected[qsizetype(cell.y()) * request.segmentation.columns + cell.x()])
                result.placements.push_back(cell);
        }
    }

    if (!hasGrouping(request)) {
        result.models.reserve(static_cast<qsizetype>(result.placements.size()));
        result.modelIds.reserve(static_cast<qsizetype>(result.placements.size()));
        for (qsizetype index = 0;
             index < static_cast<qsizetype>(result.placements.size()); ++index) {
            result.models.push_back(result.placements[static_cast<size_t>(index)]);
            result.modelIds.push_back(static_cast<int>(index));
        }
        return result;
    }

    if (!selectedOnly) {
        result.models.reserve(request.grouping.groups.size());
        for (const ElementGroup& group : request.grouping.groups)
            result.models.push_back(group.representativeCell);
        result.modelIds.reserve(static_cast<qsizetype>(result.placements.size()));
        for (const QPoint& cell : result.placements)
            result.modelIds.push_back(request.grouping.groupIdAt(cell.y(), cell.x()));
        return result;
    }

    QHash<int, int> selectedGroupIds;
    result.modelIds.reserve(static_cast<qsizetype>(result.placements.size()));
    for (const QPoint& cell : result.placements) {
        const int sourceGroupId = request.grouping.groupIdAt(cell.y(), cell.x());
        auto it = selectedGroupIds.constFind(sourceGroupId);
        if (it == selectedGroupIds.cend()) {
            const int compactId = result.models.size();
            selectedGroupIds.insert(sourceGroupId, compactId);
            result.models.push_back(cell);
            result.modelIds.push_back(compactId);
        } else {
            result.modelIds.push_back(*it);
        }
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
                  const PackageLayout& layout, const CancellationTokenPtr& token,
                  QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法创建 job.json：%1").arg(file.errorString());
        return false;
    }
    if (!writeAll(file, QByteArrayLiteral("{\n  \"models\": [\n"), error)) return false;
    const qint64 models = layout.models.size();
    for (qint64 i = 0; i < models; ++i) {
        if (token && token->isCancelled()) return false;
        const QByteArray item = QStringLiteral(
            "    {\n      \"label\": \"%1\",\n      \"path\": \"%1\"\n    }%2\n")
            .arg(i).arg(i + 1 < models ? QStringLiteral(",") : QString()).toUtf8();
        if (!writeAll(file, item, error)) return false;
    }
    if (!writeAll(file, QByteArrayLiteral("  ],\n  \"commands\": [\n"), error)) return false;
    const JobPlacementOptions placement{request.options.pixelScale, request.options.reverseY};
    const QString moveCommand = request.options.jobMoveCommand == JobMoveCommand::RelMove
        ? QStringLiteral("relmove") : QStringLiteral("absmove");
    const QPointF origin = request.selectedCells.size() > 1 && !layout.placements.empty()
        ? SplitExporter::physicalPosition(request.segmentation, layout.placements.front(),
                                          placement)
        : QPointF{};
    for (qsizetype i = 0; i < static_cast<qsizetype>(layout.placements.size()); ++i) {
        if (token && token->isCancelled()) return false;
        const QPointF position = SplitExporter::physicalPosition(
            request.segmentation, layout.placements[static_cast<size_t>(i)], placement) - origin;
        const int modelId = layout.modelIds[i];
        const QByteArray item = QStringLiteral(
            "    {\n      \"command\": \"%1\",\n      \"by\": [%2, %3, 0.0]\n    },\n"
            "    {\n      \"command\": \"place\",\n      \"id\": %4\n    }%5\n")
            .arg(moveCommand, jsonNumber(position.x()), jsonNumber(position.y()))
            .arg(modelId)
            .arg(i + 1 < static_cast<qsizetype>(layout.placements.size()) ? QStringLiteral(",")
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

bool writeParameters(const QString& path, qint64 blocks, int layers, double layerHeight,
                     QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法创建 param.txt：%1").arg(file.errorString());
        return false;
    }
    const QByteArray bytes = QStringLiteral("%1;%2;%3\n")
        .arg(blocks).arg(layers).arg(jsonNumber(layerHeight)).toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("无法保存 param.txt：%1").arg(file.errorString());
        return false;
    }
    return true;
}

bool writeReport(const QString& path, const LayeredPackageRequest& request, int layers,
                 const PackageLayout& layout, qint64 estimatedBytes, qint64 overwritten,
                 QString* error) {
    const auto strategy = request.options.layerCountStrategy == LayerCountStrategy::RoundUp
        ? QStringLiteral("round_up") : QStringLiteral("require_exact");
    const auto sampling = request.options.transformSampling == BinaryTransformSampling::Nearest
        ? QStringLiteral("nearest") : QStringLiteral("smooth_threshold");
    const qint64 cells = static_cast<qint64>(layout.placements.size());
    const qint64 groups = layout.models.size();
    const int totalLayers = layers + request.options.baseLayerCount;
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
            {QStringLiteral("zOffset"), request.options.zOffset},
            {QStringLiteral("count"), totalLayers},
            {QStringLiteral("baseCount"), request.options.baseLayerCount},
            {QStringLiteral("sliceCount"), layers},
            {QStringLiteral("strategy"), strategy},
            {QStringLiteral("invertMask"), request.options.invertMask}}},
        {QStringLiteral("output"), QJsonObject{
            {QStringLiteral("directoryCount"), groups},
            {QStringLiteral("bmpCount"), groups * totalLayers},
            {QStringLiteral("placementCount"), cells},
            {QStringLiteral("jobMoveCommand"), request.options.jobMoveCommand == JobMoveCommand::AbsMove
                ? QStringLiteral("absmove") : QStringLiteral("relmove")},
            {QStringLiteral("uniquePatternCount"), groups},
            {QStringLiteral("reusedPlacementCount"), cells - groups},
            {QStringLiteral("groupingStrategy"), hasGrouping(request)
                ? request.grouping.strategyId : QStringLiteral("none")},
            {QStringLiteral("standardWidth"), request.options.outputWidth},
            {QStringLiteral("standardHeight"), request.options.outputHeight},
            {QStringLiteral("exportMultiplier"), request.options.exportMultiplier},
            {QStringLiteral("residualAlignment"), QStringLiteral("top_left")},
            {QStringLiteral("attachToCanvas"), request.options.attachToCanvas},
            {QStringLiteral("canvasWidth"), request.options.canvasWidth},
            {QStringLiteral("canvasHeight"), request.options.canvasHeight},
            {QStringLiteral("canvasBackground"), request.options.canvasBackgroundWhite
                ? QStringLiteral("white") : QStringLiteral("black")},
            {QStringLiteral("invertCanvas"), request.options.invertCanvas},
            {QStringLiteral("rotationDegrees"), request.options.rotationDegrees},
            {QStringLiteral("scaleX"), request.options.scaleX},
            {QStringLiteral("scaleY"), request.options.scaleY},
            {QStringLiteral("mirrorX"), request.options.mirrorX},
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
            name.compare(QString::fromLatin1(kParameterName), Qt::CaseInsensitive) == 0 ||
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
        !std::isfinite(options.layerHeight) || options.layerHeight <= 0.0 ||
        !std::isfinite(options.zOffset) || options.zOffset < 0.0) {
        if (error) *error = QStringLiteral("最大高度和层切高必须是有效的正数，Z 偏移不能为负数。");
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
    const int sliceLayers = static_cast<int>(layers);
    if (options.baseLayerCount < 0 ||
        options.baseLayerCount > kMaximumMaskSliceLayers - sliceLayers) {
        if (error) {
            *error = options.baseLayerCount < 0
                ? QStringLiteral("底座数量不能小于 0。")
                : QStringLiteral("原切片与底座的总层数不能超过 %1 层。")
                      .arg(kMaximumMaskSliceLayers);
        }
        return 0;
    }
    return sliceLayers;
}

ExportSizeEstimate LayeredPackageExporter::estimateOutput(const LayeredPackageRequest& request) {
    ExportSizeEstimate result;
    const int layers = layerCount(request.options, &result.error);
    const int totalLayers = layers + request.options.baseLayerCount;
    if (!request.document || !request.document->image || layers <= 0 ||
        request.segmentation.rows <= 0 || request.segmentation.columns <= 0 ||
        request.segmentation.settings.cellWidth <= 0 ||
        request.segmentation.settings.cellHeight <= 0 || request.options.outputWidth <= 0 ||
        request.options.outputHeight <= 0 || !std::isfinite(request.options.pixelScale) ||
        request.options.pixelScale <= 0.0) {
        if (result.error.isEmpty()) result.error = QStringLiteral("分层导出参数无效。");
        return result;
    }
    const PackageLayout layout = packageLayout(request);
    const QVector<QPoint>& exports = layout.models;
    if (layout.placements.empty() || exports.isEmpty() ||
        layout.modelIds.size() != static_cast<qsizetype>(layout.placements.size())) {
        result.error = QStringLiteral("没有可导出的选中块。");
        return result;
    }
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
            !checkedMultiply(static_cast<qint64>(one), totalLayers, &block) ||
            !checkedAdd(bytes, block, &bytes)) {
            result.error = QStringLiteral("BMP 输出尺寸超过程序可表示范围。");
            return result;
        }
    }
    const qint64 uniquePatterns = exports.size();
    if (!checkedMultiply(uniquePatterns, totalLayers, &result.bmpFileCount)) {
        result.error = QStringLiteral("BMP 文件数量超过程序可表示范围。");
        return result;
    }
    result.estimatedBmpBytes = bytes;
    qint64 jobEstimate = 0;
    qint64 auxiliary = request.writeReport ? kReportEstimate : 0;
    if (!checkedMultiply(static_cast<qint64>(layout.placements.size()), 220, &jobEstimate) ||
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
    const int totalLayers = layers + request.options.baseLayerCount;
    const PackageLayout layout = packageLayout(request);
    const qint64 cells = layout.models.size();
    result.firstOutput = QStringLiteral("0/1.bmp");
    result.lastOutput = QStringLiteral("%1/%2.bmp").arg(cells - 1).arg(totalLayers);
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
    const int totalLayers = layers + request.options.baseLayerCount;
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
    const PackageLayout layout = packageLayout(request);
    const QVector<QPoint>& exports = layout.models;
    const qint64 total = static_cast<qint64>(exports.size()) + 2;
    for (qsizetype index = 0; index < exports.size(); ++index) {
        const QString directoryName = QString::number(index);
        if (token->isCancelled() || !staged.mkdir(directoryName)) {
            cleanup();
            if (!token->isCancelled() && error)
                *error = QStringLiteral("无法创建分块目录：%1").arg(directoryName);
            return false;
        }
    }
    const int workers = exportWorkerCount(request, exports.size());
    QThreadPool exportPool;
    exportPool.setMaxThreadCount(workers);
    std::atomic<qsizetype> nextIndex{0};
    std::atomic<qint64> completedGroups{0};
    std::atomic_bool stop{false};
    QMutex errorMutex;
    QMutex progressMutex;
    qsizetype firstErrorIndex = std::numeric_limits<qsizetype>::max();
    QString firstError;
    const auto exportWorker = [&] {
        while (!stop.load(std::memory_order_relaxed) && !token->isCancelled()) {
            const qsizetype index = nextIndex.fetch_add(1, std::memory_order_relaxed);
            if (index >= exports.size()) return;
            const QPoint cell = exports[index];
            const ImageRegion region = segmentationCellRegion(
                request.segmentation, request.document->metadata.width,
                request.document->metadata.height, cell.y(), cell.x());
            const LayeredMaskRequest slice{request.document, region,
                {request.segmentation.settings.cellWidth,
                 request.segmentation.settings.cellHeight}, request.options, layers};
            QString sliceError;
            if (!LayeredMaskProcessor::exportAll(
                    staged.filePath(QString::number(index)), slice, token, {}, &sliceError)) {
                if (!token->isCancelled()) {
                    QMutexLocker lock(&errorMutex);
                    if (index < firstErrorIndex) {
                        firstErrorIndex = index;
                        firstError = QStringLiteral("分块 %1 导出失败：%2")
                                         .arg(index).arg(sliceError);
                    }
                }
                stop.store(true, std::memory_order_relaxed);
                return;
            }
            const qint64 done = completedGroups.fetch_add(1, std::memory_order_relaxed) + 1;
            if (progress) {
                QMutexLocker lock(&progressMutex);
                progress({QStringLiteral("导出图元组 %1/%2").arg(done).arg(exports.size()),
                          done, total});
            }
        }
    };
    QVector<QFuture<void>> futures;
    futures.reserve(workers);
    for (int worker = 0; worker < workers; ++worker)
        futures.push_back(QtConcurrent::run(&exportPool, exportWorker));
    for (auto& future : futures) future.waitForFinished();
    exportPool.waitForDone();
    if (token->isCancelled()) { cleanup(); return false; }
    if (!firstError.isEmpty()) {
        cleanup();
        if (error) *error = firstError;
        return false;
    }
    if (!writeJobJson(staged.filePath(QStringLiteral("job.json")), request, layout, token,
                      error)) { cleanup(); return false; }
    if (!writeParameters(staged.filePath(QString::fromLatin1(kParameterName)),
                         exports.size(), totalLayers, request.options.layerHeight, error)) {
        cleanup();
        return false;
    }
    if (progress) progress({QStringLiteral("生成 job.json"), qint64(exports.size()) + 1, total});
    if (request.writeReport &&
        !writeReport(staged.filePath(QString::fromLatin1(kReportName)), request, layers,
                     layout, estimate.estimatedBmpBytes, conflicts, error)) {
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
            info.fileName().compare(QString::fromLatin1(kParameterName),
                                    Qt::CaseInsensitive) == 0 ||
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
             QString::fromLatin1(kParameterName),
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

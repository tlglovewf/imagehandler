#include "io/SplitExporter.h"
#include "io/BmpStreamWriter.h"
#include "io/ExportPreflight.h"
#include "io/PngWriter.h"
#include "imaging/VipsSupport.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>
#include <new>
#include <vips/vips.h>

namespace {

constexpr auto kExportReportName = "export-report.json";

QString pixelKindName(PixelKind kind) {
    switch (kind) {
    case PixelKind::Binary1: return QStringLiteral("binary1");
    case PixelKind::Gray8: return QStringLiteral("gray8");
    case PixelKind::Gray16: return QStringLiteral("gray16");
    case PixelKind::Color: return QStringLiteral("color");
    case PixelKind::Unsupported: return QStringLiteral("unsupported");
    }
    return QStringLiteral("unsupported");
}

bool writeExportReport(const QString& path, const ExportRequest& request,
                       qint64 estimatedBmpBytes, qint64 overwrittenTargets,
                       QString* error) {
    QString sourceName;
    if (request.document) {
        sourceName = QFileInfo(request.document->sourcePath).fileName();
        if (sourceName.isEmpty()) sourceName = QFileInfo(request.document->displayName).fileName();
    }
    const QString applicationVersion = QCoreApplication::applicationVersion().isEmpty()
        ? QStringLiteral(IMAGE_HANDLER_VERSION) : QCoreApplication::applicationVersion();
    QJsonObject application{{QStringLiteral("name"), QStringLiteral("ImageHandler")},
                            {QStringLiteral("version"), applicationVersion}};
    const auto& metadata = request.document->metadata;
    QJsonObject source{{QStringLiteral("name"), sourceName},
                       {QStringLiteral("width"), metadata.width},
                       {QStringLiteral("height"), metadata.height},
                       {QStringLiteral("pixelKind"), pixelKindName(metadata.kind)},
                       {QStringLiteral("sourceBitsPerPixel"), metadata.sourceBitsPerPixel}};
    const auto& settings = request.segmentation.settings;
    QJsonObject segmentation{{QStringLiteral("cellWidth"), settings.cellWidth},
                             {QStringLiteral("cellHeight"), settings.cellHeight},
                             {QStringLiteral("rows"), request.segmentation.rows},
                             {QStringLiteral("columns"), request.segmentation.columns},
                             {QStringLiteral("threshold"), settings.threshold},
                             {QStringLiteral("polarity"), settings.polarity == ForegroundPolarity::Dark
                                  ? QStringLiteral("dark") : QStringLiteral("light")}};
    const qint64 cells = qint64(request.segmentation.rows) * request.segmentation.columns;
    const qint64 bmpCount = request.mode == ExportMode::PrintPackage
        ? cells * request.printOptions.repeatTimes : cells;
    QJsonObject output{{QStringLiteral("bmpCount"), bmpCount},
                       {QStringLiteral("estimatedBmpBytes"), estimatedBmpBytes},
                       {QStringLiteral("overwrittenTargetCount"), overwrittenTargets},
                       {QStringLiteral("indexBase"), request.mode == ExportMode::PrintPackage ? 0 : 1},
                       {QStringLiteral("indexEnd"), request.mode == ExportMode::PrintPackage
                            ? cells - 1 : cells},
                       {QStringLiteral("namingRule"), request.mode == ExportMode::PrintPackage
                            ? QStringLiteral("{placeId}/{copy}.bmp")
                            : QStringLiteral("{index}.bmp")}};
    QJsonObject root{{QStringLiteral("schemaVersion"), 1},
                     {QStringLiteral("application"), application},
                     {QStringLiteral("createdAtUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                     {QStringLiteral("mode"), request.mode == ExportMode::PrintPackage
                          ? QStringLiteral("print_package") : QStringLiteral("split_tiles")},
                     {QStringLiteral("source"), source},
                     {QStringLiteral("segmentation"), segmentation},
                     {QStringLiteral("traversal"), request.traversalMode == TraversalMode::SnakeZ
                          ? QStringLiteral("snake_z") : QStringLiteral("clockwise_spiral")},
                     {QStringLiteral("output"), output}};
    if (request.mode == ExportMode::PrintPackage) {
        root.insert(QStringLiteral("print"), QJsonObject{
            {QStringLiteral("canvasX"), request.printOptions.canvasX},
            {QStringLiteral("canvasY"), request.printOptions.canvasY},
            {QStringLiteral("reverseY"), request.printOptions.reverseY},
            {QStringLiteral("repeatTimes"), request.printOptions.repeatTimes}});
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法创建导出报告：%1").arg(file.errorString());
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        if (error) *error = QStringLiteral("无法保存导出报告：%1").arg(file.errorString());
        return false;
    }
    return true;
}

bool verifyTargetFingerprint(const ExportRequest& request, const QString& expected,
                             qint64* conflicts, QString* error,
                             const CancellationTokenPtr& token) {
    if (expected.isEmpty()) return true;
    const QString actual = ExportPreflight::targetFingerprint(request, conflicts, nullptr,
                                                               error, token);
    if (!error->isEmpty() || (token && token->isCancelled())) return false;
    if (actual != expected) {
        *error = QStringLiteral("输出目录内容在预检后发生变化，请重新执行导出预检。");
        return false;
    }
    return true;
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

bool writeJobJson(const QString& path, const ImageMetadata& metadata,
                  const SegmentationResult& segmentation,
                  const std::vector<QPoint>& order, const PrintExportOptions& options,
                  const CancellationTokenPtr& token,
                  const std::function<void(qint64)>& itemProgress, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法创建 job.json：%1").arg(file.errorString());
        return false;
    }
    if (!writeAll(file, QByteArrayLiteral("{\n  \"models\": [\n"), error)) return false;
    for (qsizetype i = 0; i < static_cast<qsizetype>(order.size()); ++i) {
        if (token->isCancelled()) return false;
        const QByteArray item = QStringLiteral("    {\n      \"label\": \"%1\",\n"
                                               "      \"path\": \"%1\"\n    }%2\n")
                                    .arg(i)
                                    .arg(i + 1 < static_cast<qsizetype>(order.size())
                                             ? QStringLiteral(",") : QString())
                                    .toUtf8();
        if (!writeAll(file, item, error)) return false;
        if (itemProgress) itemProgress(i + 1);
    }
    if (!writeAll(file, QByteArrayLiteral("  ],\n  \"commands\": [\n"), error)) return false;
    for (qsizetype i = 0; i < static_cast<qsizetype>(order.size()); ++i) {
        if (token->isCancelled()) return false;
        const QPointF position = SplitExporter::canvasPosition(
            metadata, segmentation, order[static_cast<size_t>(i)], options);
        const QByteArray item = QStringLiteral(
            "    {\n      \"command\": \"relmove\",\n"
            "      \"by\": [%1, %2, 0.0]\n    },\n"
            "    {\n      \"command\": \"place\",\n"
            "      \"id\": %3\n    }%4\n")
                                    .arg(jsonNumber(position.x()), jsonNumber(position.y()))
                                    .arg(i)
                                    .arg(i + 1 < static_cast<qsizetype>(order.size())
                                             ? QStringLiteral(",") : QString())
                                    .toUtf8();
        if (!writeAll(file, item, error)) return false;
        if (itemProgress) itemProgress(qint64(order.size()) + i + 1);
    }
    if (!writeAll(file, QByteArrayLiteral("  ]\n}\n"), error)) return false;
    if (!file.flush()) {
        if (error) *error = QStringLiteral("无法保存 job.json：%1").arg(file.errorString());
        return false;
    }
    file.close();
    if (file.error() != QFileDevice::NoError) {
        if (error) *error = QStringLiteral("无法关闭 job.json：%1").arg(file.errorString());
        return false;
    }
    return true;
}

bool removeEntry(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink() && !info.isJunction()) return true;
    if (info.isSymLink() || info.isJunction() || info.isFile()) return QFile::remove(path);
    if (info.isDir()) return QDir(path).removeRecursively();
    return false;
}

bool isSafeManagedTarget(const QFileInfo& info, bool expectDirectory) {
    if (info.isSymLink() || info.isJunction()) return false;
    return expectDirectory ? info.isDir() : info.isFile();
}

QString selectedTileSuffix(const ImageDocumentPtr& document) {
    if (document) {
        const QString suffix = QFileInfo(document->sourcePath).suffix().toLower();
        static const QSet<QString> supportedSuffixes{
            QStringLiteral("bmp"), QStringLiteral("png"),
            QStringLiteral("tif"), QStringLiteral("tiff"),
            QStringLiteral("jpg"), QStringLiteral("jpeg")};
        if (supportedSuffixes.contains(suffix)) return QLatin1Char('.') + suffix;
        if (document->metadata.kind == PixelKind::Gray16) return QStringLiteral(".png");
    }
    return QStringLiteral(".bmp");
}

bool writeSelectedTile(const QString& path, const ImageDocumentPtr& document,
                       const ImageRegion& region, const SegmentationSettings& settings,
                       const CancellationTokenPtr& token,
                       const BmpStreamWriter::RowProgress& progress, QString* error) {
    const QString suffix = QFileInfo(path).suffix();
    if (suffix.compare(QStringLiteral("bmp"), Qt::CaseInsensitive) == 0) {
        return BmpStreamWriter::write(
            path, document, static_cast<int>(region.x), static_cast<int>(region.y),
            static_cast<int>(region.width), static_cast<int>(region.height), settings,
            progress, error);
    }
    if (suffix.compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0) {
        return PngWriter::write(
            path, document, static_cast<int>(region.x), static_cast<int>(region.y),
            static_cast<int>(region.width), static_cast<int>(region.height), token,
            progress, error);
    }

    VipsImage* croppedRaw = nullptr;
    if (vips_crop(document->image.get(), &croppedRaw,
                  static_cast<int>(region.x), static_cast<int>(region.y),
                  static_cast<int>(region.width), static_cast<int>(region.height), nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return false;
    }
    const auto cropped = VipsSupport::adopt(croppedRaw);
    const QByteArray encodedPath = QFile::encodeName(path);
    if (vips_image_write_to_file(cropped.get(), encodedPath.constData(), nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return false;
    }
    return !progress || progress(1, 1);
}

} // namespace

std::vector<QPoint> SplitExporter::traversal(int rows, int columns, TraversalMode mode) {
    std::vector<QPoint> order;
    if (rows <= 0 || columns <= 0) return order;
    const qint64 count = qint64(rows) * columns;
    if (count <= 0 || quint64(count) > order.max_size()) return order;
    try {
        order.reserve(static_cast<size_t>(count));
    } catch (const std::bad_alloc&) {
        return {};
    }
    if (mode == TraversalMode::SnakeZ) {
        for (int r = 0; r < rows; ++r) {
            if (r % 2 == 0) for (int c = 0; c < columns; ++c) order.emplace_back(c, r);
            else for (int c = columns - 1; c >= 0; --c) order.emplace_back(c, r);
        }
        return order;
    }
    int left=0,right=columns-1,top=0,bottom=rows-1;
    while(left<=right&&top<=bottom) {
        for(int c=left;c<=right;++c) order.emplace_back(c,top); ++top;
        for(int r=top;r<=bottom;++r) order.emplace_back(right,r); --right;
        if(top<=bottom) { for(int c=right;c>=left;--c) order.emplace_back(c,bottom); --bottom; }
        if(left<=right) { for(int r=bottom;r>=top;--r) order.emplace_back(left,r); ++left; }
    }
    return order;
}

qint64 SplitExporter::traversalIndex(int rows, int columns, TraversalMode mode,
                                     int row, int column) {
    if (rows <= 0 || columns <= 0 || row < 0 || row >= rows ||
        column < 0 || column >= columns) {
        return -1;
    }
    if (mode == TraversalMode::SnakeZ) {
        return qint64(row) * columns +
               (row % 2 == 0 ? column : columns - 1 - column);
    }

    const int layer = std::min({row, column, rows - 1 - row, columns - 1 - column});
    const int top = layer;
    const int left = layer;
    const int bottom = rows - 1 - layer;
    const int right = columns - 1 - layer;
    const qint64 innerRows = rows - 2LL * layer;
    const qint64 innerColumns = columns - 2LL * layer;
    const qint64 prefix = qint64(rows) * columns - innerRows * innerColumns;
    const qint64 width = right - left + 1LL;
    const qint64 height = bottom - top + 1LL;

    if (row == top) return prefix + column - left;
    if (column == right) return prefix + width + row - top - 1;
    if (row == bottom) {
        return prefix + width + height - 1 + right - column - 1;
    }
    return prefix + width + height - 1 + width - 1 + bottom - row - 1;
}

QPoint SplitExporter::nextTraversalCell(int rows, int columns, TraversalMode mode,
                                        int row, int column) {
    if (traversalIndex(rows, columns, mode, row, column) < 0) return {-1, -1};
    if (mode == TraversalMode::SnakeZ) {
        if (row % 2 == 0) {
            if (column + 1 < columns) return {column + 1, row};
        } else if (column > 0) {
            return {column - 1, row};
        }
        return row + 1 < rows ? QPoint(column, row + 1) : QPoint(-1, -1);
    }

    const int layer = std::min({row, column, rows - 1 - row, columns - 1 - column});
    const int top = layer;
    const int left = layer;
    const int bottom = rows - 1 - layer;
    const int right = columns - 1 - layer;
    if (row == top) {
        if (column < right) return {column + 1, row};
        if (row < bottom) return {column, row + 1};
        return {-1, -1};
    }
    if (column == right) {
        if (row < bottom) return {column, row + 1};
        if (column > left) return {column - 1, row};
        return {-1, -1};
    }
    if (row == bottom) {
        if (column > left) return {column - 1, row};
        if (row > top + 1) return {column, row - 1};
    } else if (column == left && row > top + 1) {
        return {column, row - 1};
    }
    if (top + 1 <= bottom - 1 && left + 1 <= right - 1) {
        return {left + 1, top + 1};
    }
    return {-1, -1};
}

QPointF SplitExporter::canvasPosition(const ImageMetadata& metadata,
                                      const SegmentationResult& segmentation,
                                      const QPoint& cell,
                                      const PrintExportOptions& options) {
    if (metadata.width <= 0 || metadata.height <= 0 || cell.x() < 0 || cell.y() < 0 ||
        segmentation.settings.cellWidth <= 0 || segmentation.settings.cellHeight <= 0 ||
        options.canvasX <= 0 || options.canvasY <= 0) {
        return {};
    }
    const double x = static_cast<double>(qint64(cell.x()) * segmentation.settings.cellWidth) /
                     static_cast<double>(metadata.width) * options.canvasX;
    double y = static_cast<double>(qint64(cell.y()) * segmentation.settings.cellHeight) /
               static_cast<double>(metadata.height) * options.canvasY;
    if (options.reverseY) y = -y;
    return {x, y == 0.0 ? 0.0 : y};
}

QPointF SplitExporter::physicalPosition(const SegmentationResult& segmentation,
                                        const QPoint& cell,
                                        const JobPlacementOptions& options) {
    if (cell.x() < 0 || cell.y() < 0 ||
        segmentation.settings.cellWidth <= 0 || segmentation.settings.cellHeight <= 0 ||
        !std::isfinite(options.pixelScale) || options.pixelScale <= 0.0) {
        return {};
    }
    double pixelX = static_cast<double>(qint64(cell.x()) *
                                        segmentation.settings.cellWidth);
    double pixelY = static_cast<double>(qint64(cell.y()) *
                                        segmentation.settings.cellHeight);
    if (options.centerOrigin) {
        pixelX -= static_cast<double>(qint64(segmentation.columns - 1) *
                                      segmentation.settings.cellWidth) * 0.5;
        pixelY -= static_cast<double>(qint64(segmentation.rows - 1) *
                                      segmentation.settings.cellHeight) * 0.5;
    }
    double x = pixelX * options.pixelScale;
    double y = pixelY * options.pixelScale;
    if (options.reverseX) x = -x;
    if (options.reverseY) y = -y;
    return {x == 0.0 ? 0.0 : x, y == 0.0 ? 0.0 : y};
}

QPointF SplitExporter::physicalSelectionOrigin(
        const SegmentationResult& segmentation, const QVector<QPoint>& selectedCells,
        const JobPlacementOptions& options) {
    if (segmentation.rows <= 0 || segmentation.columns <= 0 ||
        segmentation.settings.cellWidth <= 0 || segmentation.settings.cellHeight <= 0 ||
        !std::isfinite(options.pixelScale) || options.pixelScale <= 0.0) {
        return {};
    }

    int minimumColumn = 0;
    int maximumColumn = segmentation.columns - 1;
    int minimumRow = 0;
    int maximumRow = segmentation.rows - 1;
    if (!selectedCells.isEmpty()) {
        minimumColumn = segmentation.columns;
        maximumColumn = -1;
        minimumRow = segmentation.rows;
        maximumRow = -1;
        for (const QPoint& cell : selectedCells) {
            if (cell.x() < 0 || cell.y() < 0 || cell.x() >= segmentation.columns ||
                cell.y() >= segmentation.rows) continue;
            minimumColumn = std::min(minimumColumn, cell.x());
            maximumColumn = std::max(maximumColumn, cell.x());
            minimumRow = std::min(minimumRow, cell.y());
            maximumRow = std::max(maximumRow, cell.y());
        }
        if (maximumColumn < minimumColumn || maximumRow < minimumRow) return {};
    }

    const JobPlacementOptions absoluteOptions{
        options.pixelScale, options.reverseY, false, options.reverseX};
    const QPointF minimum = physicalPosition(
        segmentation, {minimumColumn, minimumRow}, absoluteOptions);
    const QPointF maximum = physicalPosition(
        segmentation, {maximumColumn, maximumRow}, absoluteOptions);
    const double x = (minimum.x() + maximum.x()) * 0.5;
    const double y = (minimum.y() + maximum.y()) * 0.5;
    return {x == 0.0 ? 0.0 : x, y == 0.0 ? 0.0 : y};
}

QString SplitExporter::selectedTileFileName(const ImageDocumentPtr& document,
                                             qint64 traversalIndex) {
    if (traversalIndex < 0) return {};
    return QString::number(traversalIndex + 1) + selectedTileSuffix(document);
}

bool SplitExporter::exportTiles(const QString& outputDirectory, const ImageDocumentPtr& document,
                                const SegmentationResult& segmentation, TraversalMode mode,
                                bool overwrite, const CancellationTokenPtr& token,
                                const Progress& progress, QString* error) {
    return exportTiles(outputDirectory, document, segmentation, mode,
                       ExportExecutionOptions{overwrite, false, {}}, token, progress, error);
}

bool SplitExporter::exportTiles(const QString& outputDirectory, const ImageDocumentPtr& document,
                                const SegmentationResult& segmentation, TraversalMode mode,
                                const ExportExecutionOptions& execution,
                                const CancellationTokenPtr& token,
                                const Progress& progress, QString* error) {
    if (!document || !document->image || segmentation.settings.cellWidth <= 0 ||
        segmentation.settings.cellHeight <= 0) {
        if (error) *error = QStringLiteral("分割文档或参数无效。");
        return false;
    }
    const qint64 cellCount = qint64(segmentation.rows) * segmentation.columns;
    if (cellCount <= 0) {
        if (error) *error = QStringLiteral("分割数量无效。");
        return false;
    }
    const qint64 expectedColumns = (document->metadata.width + segmentation.settings.cellWidth - 1) /
                                   segmentation.settings.cellWidth;
    const qint64 expectedRows = (document->metadata.height + segmentation.settings.cellHeight - 1) /
                                segmentation.settings.cellHeight;
    if (segmentation.columns != expectedColumns || segmentation.rows != expectedRows) {
        if (error) *error = QStringLiteral("分割网格与当前图像尺寸不匹配。");
        return false;
    }
    QDir output(outputDirectory);
    if (!output.exists() && !output.mkpath(".")) { if(error)*error=QStringLiteral("无法创建输出目录。"); return false; }
    const ExportRequest request{ExportMode::SplitTiles, outputDirectory, document, segmentation,
                                mode, {}, execution.writeReport};
    qint64 preflightConflictCount = 0;
    QString fingerprintError;
    if (!verifyTargetFingerprint(request, execution.confirmedTargetFingerprint,
                                 &preflightConflictCount, &fingerprintError, token)) {
        if (!token->isCancelled() && error) *error = fingerprintError;
        return false;
    }
    const auto order = traversal(segmentation.rows, segmentation.columns, mode);
    if (order.empty()) { if(error)*error=QStringLiteral("没有可导出的完整单元。"); return false; }
    const QString tempName = QStringLiteral(".image_handler_%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    if (!output.mkdir(tempName)) { if(error)*error=QStringLiteral("无法创建临时导出目录。"); return false; }
    QDir temp(output.filePath(tempName));
    const auto cleanup = [&] { return temp.removeRecursively(); };
    const qint64 totalWork = qint64(order.size()) * 1001;
    qint64 lastReportedPermille = -1;
    const auto reportProgress = [&](const QString& phase, qint64 completed) {
        if (!progress) return;
        const qint64 permille = completed * 1000 / std::max<qint64>(1, totalWork);
        if (permille == lastReportedPermille && completed < totalWork) return;
        lastReportedPermille = permille;
        progress({phase, completed, totalWork});
    };

    for (qsizetype i = 0; i < static_cast<qsizetype>(order.size()); ++i) {
        if (token->isCancelled()) { cleanup(); return false; }
        const QString name = QString::number(i + 1) + QStringLiteral(".bmp");
        QString writeError;
        const QPoint cell = order[static_cast<size_t>(i)];
        const ImageRegion region = segmentationCellRegion(
            segmentation, document->metadata.width, document->metadata.height, cell.y(), cell.x());
        if (!region.isValid() || region.x > INT_MAX || region.y > INT_MAX ||
            region.width > INT_MAX || region.height > INT_MAX) {
            cleanup();
            if (error) *error = QStringLiteral("分割区域超出 BMP 导出器支持的尺寸范围。");
            return false;
        }
        const bool ok = BmpStreamWriter::write(temp.filePath(name), document,
            static_cast<int>(region.x), static_cast<int>(region.y),
            static_cast<int>(region.width), static_cast<int>(region.height), segmentation.settings,
            [&](int row, int rowTotal) {
                if (token->isCancelled()) return false;
                const qint64 base = qint64(i) * 1000;
                reportProgress(QStringLiteral("导出 %1").arg(name),
                               base + qint64(row) * 1000 / std::max(1, rowTotal));
                return true;
            }, &writeError);
        if (!ok) { cleanup(); if(!token->isCancelled()&&error)*error=writeError; return false; }
    }

    if (execution.writeReport) {
        QString estimateError;
        const qint64 estimatedBytes = ExportPreflight::estimateBmpBytes(request, &estimateError);
        if (estimatedBytes < 0 || !writeExportReport(temp.filePath(QString::fromLatin1(kExportReportName)),
                                                     request, estimatedBytes,
                                                     preflightConflictCount, error)) {
            cleanup();
            if (error && error->isEmpty()) *error = estimateError;
            return false;
        }
    }

    if (token->isCancelled()) { cleanup(); return false; }
    if (!verifyTargetFingerprint(request, execution.confirmedTargetFingerprint,
                                 nullptr, &fingerprintError, token)) {
        cleanup();
        if (!token->isCancelled() && error) *error = fingerprintError;
        return false;
    }
    QDir backup(temp.filePath("backup")); temp.mkdir("backup");
    QStringList committed, backedUp;
    for (qsizetype i = 0; i < static_cast<qsizetype>(order.size()); ++i) {
        if (token->isCancelled()) { if(error)*error=QStringLiteral("提交阶段已取消。"); goto rollback; }
        const QString name = QString::number(i + 1) + QStringLiteral(".bmp");
        const QString target = output.filePath(name);
        const QFileInfo targetInfo(target);
        if (targetInfo.exists()) {
            if (!targetInfo.isFile() || targetInfo.isSymLink()) {
                if(error)*error=QStringLiteral("目标路径不是可覆盖的普通文件：%1").arg(name);
                goto rollback;
            }
            if (!execution.overwrite) { if(error)*error=QStringLiteral("目标文件已存在：%1").arg(name); goto rollback; }
            if (!QFile::rename(target, backup.filePath(name))) { if(error)*error=QStringLiteral("无法备份：%1").arg(name); goto rollback; }
            backedUp << name;
        }
        if (!QFile::rename(temp.filePath(name), target)) { if(error)*error=QStringLiteral("无法提交：%1").arg(name); goto rollback; }
        committed << name;
        reportProgress(QStringLiteral("提交文件"), qint64(order.size()) * 1000 + i + 1);
    }
    {
        const QString name = QString::fromLatin1(kExportReportName);
        const QString target = output.filePath(name);
        const QFileInfo targetInfo(target);
        if (targetInfo.exists() || targetInfo.isSymLink() || targetInfo.isJunction()) {
            if (!targetInfo.isFile() || targetInfo.isSymLink() || targetInfo.isJunction()) {
                if (error) *error = QStringLiteral("目标路径不是可覆盖的普通文件：%1").arg(name);
                goto rollback;
            }
            if (!execution.overwrite) {
                if (error) *error = QStringLiteral("目标文件已存在：%1").arg(name);
                goto rollback;
            }
            if (!QFile::rename(target, backup.filePath(name))) {
                if (error) *error = QStringLiteral("无法备份：%1").arg(name);
                goto rollback;
            }
            backedUp << name;
        }
        if (execution.writeReport) {
            if (!QFile::rename(temp.filePath(name), target)) {
                if (error) *error = QStringLiteral("无法提交：%1").arg(name);
                goto rollback;
            }
            committed << name;
        }
    }
    cleanup(); return true;

rollback:
    {
        QStringList rollbackErrors;
        for (auto it = committed.crbegin(); it != committed.crend(); ++it) {
            const QString target = output.filePath(*it);
            if (QFile::exists(target) && !QFile::remove(target))
                rollbackErrors << QStringLiteral("无法移除已提交文件 %1").arg(*it);
        }
        for (auto it = backedUp.crbegin(); it != backedUp.crend(); ++it) {
            const QString target = output.filePath(*it);
            const QString saved = backup.filePath(*it);
            if (QFile::exists(target)) {
                rollbackErrors << QStringLiteral("目标仍被占用，未恢复 %1").arg(*it);
            } else if (!QFile::rename(saved, target)) {
                rollbackErrors << QStringLiteral("无法恢复原文件 %1").arg(*it);
            }
        }
        if (rollbackErrors.isEmpty()) {
            if (!cleanup() && error)
                *error += QStringLiteral("\n回滚已完成，但临时目录未能删除：%1").arg(temp.absolutePath());
        } else if (error) {
            *error += QStringLiteral("\n回滚未完全成功：%1。备份已保留在：%2")
                          .arg(rollbackErrors.join(QStringLiteral("；")), temp.absolutePath());
        }
        return false;
    }
}

bool SplitExporter::exportSelectedTiles(
    const QString& outputDirectory, const ImageDocumentPtr& document,
    const SegmentationResult& segmentation, TraversalMode mode,
    const QVector<QPoint>& cells, bool overwrite, const CancellationTokenPtr& token,
    const Progress& progress, QString* error) {
    if (!document || !document->image || !token || cells.isEmpty() ||
        segmentation.settings.cellWidth <= 0 || segmentation.settings.cellHeight <= 0 ||
        segmentation.rows <= 0 || segmentation.columns <= 0) {
        if (error) *error = QStringLiteral("选中块导出参数无效。");
        return false;
    }
    const qint64 expectedColumns =
        (document->metadata.width + segmentation.settings.cellWidth - 1) /
        segmentation.settings.cellWidth;
    const qint64 expectedRows =
        (document->metadata.height + segmentation.settings.cellHeight - 1) /
        segmentation.settings.cellHeight;
    if (segmentation.columns != expectedColumns || segmentation.rows != expectedRows) {
        if (error) *error = QStringLiteral("分割网格与当前图像尺寸不匹配。");
        return false;
    }

    struct SelectedTarget {
        qint64 traversalIndex = -1;
        ImageRegion region;
        QString name;
    };
    QVector<SelectedTarget> targets;
    targets.reserve(cells.size());
    QSet<QPoint> uniqueCells;
    for (const QPoint& cell : cells) {
        if (uniqueCells.contains(cell)) continue;
        uniqueCells.insert(cell);
        const qint64 index = traversalIndex(segmentation.rows, segmentation.columns, mode,
                                            cell.y(), cell.x());
        const ImageRegion region = segmentationCellRegion(
            segmentation, document->metadata.width, document->metadata.height,
            cell.y(), cell.x());
        if (index < 0 || !region.isValid() || region.x > INT_MAX || region.y > INT_MAX ||
            region.width > INT_MAX || region.height > INT_MAX) {
            if (error) *error = QStringLiteral("选中的块坐标或尺寸无效。");
            return false;
        }
        targets.push_back({index, region, selectedTileFileName(document, index)});
    }
    std::sort(targets.begin(), targets.end(), [](const SelectedTarget& left,
                                                  const SelectedTarget& right) {
        return left.traversalIndex < right.traversalIndex;
    });
    if (targets.isEmpty()) {
        if (error) *error = QStringLiteral("没有可导出的选中块。");
        return false;
    }

    QDir output(outputDirectory);
    if (!output.exists() && !output.mkpath(QStringLiteral("."))) {
        if (error) *error = QStringLiteral("无法创建输出目录。");
        return false;
    }
    const QString tempName = QStringLiteral(".image_handler_selected_%1")
        .arg(QUuid::createUuid().toString(QUuid::Id128));
    if (!output.mkdir(tempName)) {
        if (error) *error = QStringLiteral("无法创建选中块导出临时目录。");
        return false;
    }
    QDir temp(output.filePath(tempName));
    const auto cleanup = [&] { return temp.removeRecursively(); };
    const qint64 totalWork = qint64(targets.size()) * 1001;
    qint64 lastProgress = -1;
    const auto reportProgress = [&](const QString& phase, qint64 completed) {
        if (!progress || completed == lastProgress) return;
        lastProgress = completed;
        progress({phase, completed, totalWork});
    };

    for (qsizetype i = 0; i < targets.size(); ++i) {
        if (token->isCancelled()) { cleanup(); return false; }
        const SelectedTarget& target = targets[i];
        QString writeError;
        const bool written = writeSelectedTile(
            temp.filePath(target.name), document, target.region, segmentation.settings, token,
            [&](int row, int rowTotal) {
                if (token->isCancelled()) return false;
                reportProgress(QStringLiteral("导出 %1").arg(target.name),
                    qint64(i) * 1000 + qint64(row) * 1000 / std::max(1, rowTotal));
                return true;
            }, &writeError);
        if (!written) {
            cleanup();
            if (!token->isCancelled() && error) *error = writeError;
            return false;
        }
        reportProgress(QStringLiteral("导出 %1").arg(target.name), qint64(i + 1) * 1000);
    }

    if (token->isCancelled()) { cleanup(); return false; }
    temp.mkdir(QStringLiteral("backup"));
    QDir backup(temp.filePath(QStringLiteral("backup")));
    QStringList committed;
    QStringList backedUp;
    for (qsizetype i = 0; i < targets.size(); ++i) {
        const QString name = targets[i].name;
        if (token->isCancelled()) {
            if (error) *error = QStringLiteral("提交阶段已取消。");
            goto rollbackSelected;
        }
        const QString targetPath = output.filePath(name);
        const QFileInfo targetInfo(targetPath);
        if (targetInfo.exists() || targetInfo.isSymLink() || targetInfo.isJunction()) {
            if (!targetInfo.isFile() || targetInfo.isSymLink() || targetInfo.isJunction()) {
                if (error) *error = QStringLiteral("目标路径不是可替换的普通文件：%1").arg(name);
                goto rollbackSelected;
            }
            if (!overwrite) {
                if (error) *error = QStringLiteral("目标文件已存在：%1").arg(name);
                goto rollbackSelected;
            }
            if (!QFile::rename(targetPath, backup.filePath(name))) {
                if (error) *error = QStringLiteral("无法备份已有文件：%1").arg(name);
                goto rollbackSelected;
            }
            backedUp << name;
        }
        if (!QFile::rename(temp.filePath(name), targetPath)) {
            if (error) *error = QStringLiteral("无法提交文件：%1").arg(name);
            goto rollbackSelected;
        }
        committed << name;
        reportProgress(QStringLiteral("提交文件"), qint64(targets.size()) * 1000 + i + 1);
    }
    cleanup();
    return true;

rollbackSelected:
    {
        QStringList rollbackErrors;
        for (auto it = committed.crbegin(); it != committed.crend(); ++it) {
            const QString path = output.filePath(*it);
            if (QFile::exists(path) && !QFile::remove(path))
                rollbackErrors << QStringLiteral("无法移除已提交文件 %1").arg(*it);
        }
        for (auto it = backedUp.crbegin(); it != backedUp.crend(); ++it) {
            const QString path = output.filePath(*it);
            const QString saved = backup.filePath(*it);
            if (QFile::exists(path) || !QFile::rename(saved, path))
                rollbackErrors << QStringLiteral("无法恢复原文件 %1").arg(*it);
        }
        if (rollbackErrors.isEmpty()) {
            if (!cleanup() && error)
                *error += QStringLiteral("\n回滚完成，但临时目录未能删除：%1")
                              .arg(temp.absolutePath());
        } else if (error) {
            *error += QStringLiteral("\n回滚未完全成功：%1。备份保留在：%2")
                          .arg(rollbackErrors.join(QStringLiteral("；")), temp.absolutePath());
        }
        return false;
    }
}

bool SplitExporter::exportPrintPackage(const QString& outputDirectory,
                                       const ImageDocumentPtr& document,
                                       const SegmentationResult& segmentation, TraversalMode mode,
                                       const PrintExportOptions& options, bool overwrite,
                                       const CancellationTokenPtr& token,
                                       const Progress& progress, QString* error) {
    return exportPrintPackage(outputDirectory, document, segmentation, mode, options,
                              ExportExecutionOptions{overwrite, false, {}}, token, progress, error);
}

bool SplitExporter::exportPrintPackage(const QString& outputDirectory,
                                       const ImageDocumentPtr& document,
                                       const SegmentationResult& segmentation, TraversalMode mode,
                                       const PrintExportOptions& options,
                                       const ExportExecutionOptions& execution,
                                       const CancellationTokenPtr& token,
                                       const Progress& progress, QString* error) {
    if (!document || !document->image || !token || segmentation.settings.cellWidth <= 0 ||
        segmentation.settings.cellHeight <= 0 || options.canvasX <= 0 ||
        options.canvasY <= 0 || options.repeatTimes <= 0) {
        if (error) *error = QStringLiteral("打印导出文档或参数无效。");
        return false;
    }
    const qint64 cellCount = qint64(segmentation.rows) * segmentation.columns;
    if (cellCount <= 0) {
        if (error) *error = QStringLiteral("分割数量无效。");
        return false;
    }
    const qint64 workPerCell = 1003 + qint64(options.repeatTimes);
    if (cellCount > (std::numeric_limits<qint64>::max() - 1) / workPerCell) {
        if (error) *error = QStringLiteral("打印文件数量超出程序可表示范围。");
        return false;
    }
    const qint64 expectedColumns = (document->metadata.width + segmentation.settings.cellWidth - 1) /
                                   segmentation.settings.cellWidth;
    const qint64 expectedRows = (document->metadata.height + segmentation.settings.cellHeight - 1) /
                                segmentation.settings.cellHeight;
    if (segmentation.columns != expectedColumns || segmentation.rows != expectedRows) {
        if (error) *error = QStringLiteral("分割网格与当前图像尺寸不匹配。");
        return false;
    }
    const auto order = traversal(segmentation.rows, segmentation.columns, mode);
    if (order.empty()) {
        if (error) *error = QStringLiteral("没有可导出的分割单元。");
        return false;
    }

    QDir output(outputDirectory);
    if (!output.exists() && !output.mkpath(QStringLiteral("."))) {
        if (error) *error = QStringLiteral("无法创建输出目录。");
        return false;
    }

    const ExportRequest request{ExportMode::PrintPackage, outputDirectory, document,
                                segmentation, mode, options, execution.writeReport};
    qint64 preflightConflictCount = 0;
    QString fingerprintError;
    if (!verifyTargetFingerprint(request, execution.confirmedTargetFingerprint,
                                 &preflightConflictCount, &fingerprintError, token)) {
        if (!token->isCancelled() && error) *error = fingerprintError;
        return false;
    }

    const QRegularExpression numericName(QStringLiteral("^[0-9]+$"));
    QStringList existingTargets;
    const QFileInfoList entries = output.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo& info : entries) {
        const QString name = info.fileName();
        if (!numericName.match(name).hasMatch()) continue;
        if (!isSafeManagedTarget(info, true)) {
            if (error) *error = QStringLiteral("数字目标路径不是可安全替换的普通目录：%1").arg(name);
            return false;
        }
        existingTargets << name;
    }
    const QFileInfo jobInfo(output.filePath(QStringLiteral("job.json")));
    if (jobInfo.exists() || jobInfo.isSymLink()) {
        if (!isSafeManagedTarget(jobInfo, false)) {
            if (error) *error = QStringLiteral("job.json 不是可安全替换的普通文件。");
            return false;
        }
        existingTargets << QStringLiteral("job.json");
    }
    {
        const QFileInfo reportInfo(output.filePath(QString::fromLatin1(kExportReportName)));
        if (reportInfo.exists() || reportInfo.isSymLink() || reportInfo.isJunction()) {
            if (!isSafeManagedTarget(reportInfo, false)) {
                if (error) *error = QStringLiteral("export-report.json 不是可安全替换的普通文件。");
                return false;
            }
            existingTargets << QString::fromLatin1(kExportReportName);
        }
    }
    if (!existingTargets.isEmpty() && !execution.overwrite) {
        if (error) *error = QStringLiteral("PRINT_TARGETS_EXIST:%1").arg(existingTargets.size());
        return false;
    }

    const QString tempName = QStringLiteral(".image_handler_print_%1")
                                 .arg(QUuid::createUuid().toString(QUuid::Id128));
    if (!output.mkdir(tempName)) {
        if (error) *error = QStringLiteral("无法创建打印导出临时目录。");
        return false;
    }
    QDir temp(output.filePath(tempName));
    if (!temp.mkdir(QStringLiteral("staged")) || !temp.mkdir(QStringLiteral("backup"))) {
        temp.removeRecursively();
        if (error) *error = QStringLiteral("无法初始化打印导出临时目录。");
        return false;
    }
    QDir staged(temp.filePath(QStringLiteral("staged")));
    QDir backup(temp.filePath(QStringLiteral("backup")));
    const auto cleanup = [&] { return temp.removeRecursively(); };
    const qint64 totalFiles = cellCount * qint64(options.repeatTimes);
    const qint64 totalWork = cellCount * workPerCell + 1;
    qint64 completedCopies = 0;
    qint64 lastReportedPermille = -1;
    const auto reportProgress = [&](const QString& phase, qint64 completed) {
        if (!progress) return;
        const qint64 permille = completed * 1000 / std::max<qint64>(1, totalWork);
        if (permille == lastReportedPermille && completed < totalWork) return;
        lastReportedPermille = permille;
        progress({phase, completed, totalWork});
    };

    for (qsizetype i = 0; i < static_cast<qsizetype>(order.size()); ++i) {
        if (token->isCancelled()) { cleanup(); return false; }
        const QString directoryName = QString::number(i);
        if (!staged.mkdir(directoryName)) {
            cleanup();
            if (error) *error = QStringLiteral("无法创建打印分块目录：%1").arg(directoryName);
            return false;
        }
        QDir tileDirectory(staged.filePath(directoryName));
        const QPoint cell = order[static_cast<size_t>(i)];
        const ImageRegion region = segmentationCellRegion(
            segmentation, document->metadata.width, document->metadata.height,
            cell.y(), cell.x());
        if (!region.isValid() || region.x > INT_MAX || region.y > INT_MAX ||
            region.width > INT_MAX || region.height > INT_MAX) {
            cleanup();
            if (error) *error = QStringLiteral("分割区域超出 BMP 导出器支持的尺寸范围。");
            return false;
        }
        QString writeError;
        const bool written = BmpStreamWriter::write(
            tileDirectory.filePath(QStringLiteral("1.bmp")), document,
            static_cast<int>(region.x), static_cast<int>(region.y),
            static_cast<int>(region.width), static_cast<int>(region.height),
            segmentation.settings,
            [&](int row, int rowTotal) {
                if (token->isCancelled()) return false;
                reportProgress(QStringLiteral("导出分块 %1").arg(i),
                               qint64(i) * 1000 + qint64(row) * 1000 /
                                                        std::max(1, rowTotal) + completedCopies);
                return true;
            }, &writeError);
        if (!written) {
            cleanup();
            if (!token->isCancelled() && error) *error = writeError;
            return false;
        }
        ++completedCopies;
        for (qint64 copy = 2; copy <= options.repeatTimes; ++copy) {
            if (token->isCancelled()) { cleanup(); return false; }
            if (!QFile::copy(tileDirectory.filePath(QStringLiteral("1.bmp")),
                             tileDirectory.filePath(QString::number(copy) +
                                                    QStringLiteral(".bmp")))) {
                cleanup();
                if (error) *error = QStringLiteral("无法复制分块 %1 的第 %2 份 BMP。").arg(i).arg(copy);
                return false;
            }
            ++completedCopies;
            reportProgress(QStringLiteral("复制分块 %1").arg(i),
                           qint64(i + 1) * 1000 + completedCopies);
        }
    }

    if (!writeJobJson(staged.filePath(QStringLiteral("job.json")), document->metadata,
                      segmentation, order, options, token,
                      [&](qint64 done) {
                          reportProgress(QStringLiteral("生成 job.json"),
                                         cellCount * 1000 + totalFiles + done);
                      }, error)) {
        cleanup();
        return false;
    }
    if (execution.writeReport) {
        QString estimateError;
        const qint64 estimatedBytes = ExportPreflight::estimateBmpBytes(request, &estimateError);
        if (estimatedBytes < 0 ||
            !writeExportReport(staged.filePath(QString::fromLatin1(kExportReportName)), request,
                               estimatedBytes, preflightConflictCount, error)) {
            cleanup();
            if (error && error->isEmpty()) *error = estimateError;
            return false;
        }
    }
    if (token->isCancelled()) { cleanup(); return false; }

    if (!verifyTargetFingerprint(request, execution.confirmedTargetFingerprint,
                                 nullptr, &fingerprintError, token)) {
        cleanup();
        if (!token->isCancelled() && error) *error = fingerprintError;
        return false;
    }

    QStringList backedUp;
    QStringList committed;
    for (const QString& name : existingTargets) {
        if (token->isCancelled()) {
            if (error) *error = QStringLiteral("提交阶段已取消。");
            goto rollback_print;
        }
        if (!QDir().rename(output.filePath(name), backup.filePath(name))) {
            if (error) *error = QStringLiteral("无法备份旧打印结果：%1").arg(name);
            goto rollback_print;
        }
        backedUp << name;
    }
    for (qsizetype i = 0; i < static_cast<qsizetype>(order.size()); ++i) {
        const QString name = QString::number(i);
        if (token->isCancelled()) {
            if (error) *error = QStringLiteral("提交阶段已取消。");
            goto rollback_print;
        }
        if (!QDir().rename(staged.filePath(name), output.filePath(name))) {
            if (error) *error = QStringLiteral("无法提交打印分块目录：%1").arg(name);
            goto rollback_print;
        }
        committed << name;
        reportProgress(QStringLiteral("提交打印结果"),
                       cellCount * 1000 + totalFiles + cellCount * 2 + i + 1);
    }
    if (!QDir().rename(staged.filePath(QStringLiteral("job.json")),
                       output.filePath(QStringLiteral("job.json")))) {
        if (error) *error = QStringLiteral("无法提交 job.json。");
        goto rollback_print;
    }
    committed << QStringLiteral("job.json");
    if (execution.writeReport) {
        const QString name = QString::fromLatin1(kExportReportName);
        if (!QDir().rename(staged.filePath(name), output.filePath(name))) {
            if (error) *error = QStringLiteral("无法提交导出报告。");
            goto rollback_print;
        }
        committed << name;
    }
    reportProgress(QStringLiteral("提交打印结果"), totalWork);
    cleanup();
    return true;

rollback_print:
    {
        QStringList rollbackErrors;
        for (auto it = committed.crbegin(); it != committed.crend(); ++it) {
            if (!removeEntry(output.filePath(*it)))
                rollbackErrors << QStringLiteral("无法移除新结果 %1").arg(*it);
        }
        for (auto it = backedUp.crbegin(); it != backedUp.crend(); ++it) {
            const QString target = output.filePath(*it);
            if (QFileInfo::exists(target)) {
                rollbackErrors << QStringLiteral("目标仍被占用，未恢复 %1").arg(*it);
            } else if (!QDir().rename(backup.filePath(*it), target)) {
                rollbackErrors << QStringLiteral("无法恢复旧结果 %1").arg(*it);
            }
        }
        if (rollbackErrors.isEmpty()) {
            if (!cleanup() && error)
                *error += QStringLiteral("\n回滚已完成，但临时目录未能删除：%1")
                              .arg(temp.absolutePath());
        } else if (error) {
            *error += QStringLiteral("\n回滚未完全成功：%1。备份已保留在：%2")
                          .arg(rollbackErrors.join(QStringLiteral("；")), temp.absolutePath());
        }
        return false;
    }
}

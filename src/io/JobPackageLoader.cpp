#include "io/JobPackageLoader.h"

#include "domain/MaskSliceModels.h"
#include "imaging/JobStackLod.h"
#include "imaging/MaskStackMeshBuilder.h"
#include "io/JobFileLoader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <set>

namespace {
constexpr int kMaximumStackPreviewSide = 512;
constexpr qint64 kTargetRenderedCellInstances = 1'000'000;
constexpr std::size_t kTargetExplodedQuadInstances = 250'000;

struct PackageSettings {
    double pixelScale = 0.0;
    double layerHeight = 0.0;
    int layerCount = 0;
    QSize outputSize;
    bool invertMask = false;
};

bool positiveFinite(const QJsonValue& value, double* result) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number <= 0.0) return false;
    *result = number;
    return true;
}

bool positiveInteger(const QJsonValue& value, int maximum, int* result) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 1.0 || number > maximum ||
        std::floor(number) != number) return false;
    *result = static_cast<int>(number);
    return true;
}

bool readPackageSettings(const QString& path, PackageSettings* settings, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取 export-report.json：%1").arg(file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (json.isNull() || !json.isObject()) {
        *error = QStringLiteral("export-report.json 格式错误：%1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = json.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt(-1) != 3 ||
        root.value(QStringLiteral("mode")).toString() != QStringLiteral("layered_package")) {
        *error = QStringLiteral("export-report.json 不是当前版本的 ImageHandler 分层包报告。");
        return false;
    }
    const QJsonObject mapping = root.value(QStringLiteral("mapping")).toObject();
    const QJsonObject layers = root.value(QStringLiteral("layers")).toObject();
    const QJsonObject output = root.value(QStringLiteral("output")).toObject();
    if (!positiveFinite(mapping.value(QStringLiteral("pixel2phy")), &settings->pixelScale) ||
        !positiveFinite(layers.value(QStringLiteral("layerHeight")), &settings->layerHeight) ||
        !positiveInteger(layers.value(QStringLiteral("count")), kMaximumMaskSliceLayers,
                         &settings->layerCount) ||
        !layers.value(QStringLiteral("invertMask")).isBool() ||
        !output.value(QStringLiteral("attachToCanvas")).isBool() ||
        output.value(QStringLiteral("bitsPerPixel")).toInt(-1) != 1) {
        *error = QStringLiteral("export-report.json 缺少有效的分层尺度、层数或位深信息。");
        return false;
    }
    settings->invertMask = layers.value(QStringLiteral("invertMask")).toBool();
    const bool attached = output.value(QStringLiteral("attachToCanvas")).toBool();
    int width = 0, height = 0;
    const QString widthKey = attached ? QStringLiteral("canvasWidth")
                                      : QStringLiteral("standardWidth");
    const QString heightKey = attached ? QStringLiteral("canvasHeight")
                                       : QStringLiteral("standardHeight");
    if (!positiveInteger(output.value(widthKey), std::numeric_limits<int>::max(), &width) ||
        !positiveInteger(output.value(heightKey), std::numeric_limits<int>::max(), &height)) {
        *error = QStringLiteral("export-report.json 缺少有效的切片输出尺寸。");
        return false;
    }
    settings->outputSize = {width, height};
    const double physicalWidth = width * settings->pixelScale;
    const double physicalHeight = height * settings->pixelScale;
    const double physicalDepth = settings->layerCount * settings->layerHeight;
    if (!std::isfinite(physicalWidth) || !std::isfinite(physicalHeight) ||
        !std::isfinite(physicalDepth) ||
        physicalWidth > std::numeric_limits<float>::max() ||
        physicalHeight > std::numeric_limits<float>::max() ||
        physicalDepth > std::numeric_limits<float>::max()) {
        *error = QStringLiteral("分层包的物理尺寸超出渲染范围。");
        return false;
    }
    return true;
}

bool isInsideRoot(const QDir& root, const QString& relativePath, QString* absolutePath) {
    if (relativePath.trimmed().isEmpty() || QDir::isAbsolutePath(relativePath)) return false;
    const QString clean = QDir::cleanPath(relativePath);
    if (clean == QStringLiteral("..") || clean.startsWith(QStringLiteral("../")) ||
        clean.startsWith(QStringLiteral("..\\"))) return false;
    const QFileInfo candidateInfo(root.filePath(clean));
    const QString candidate = candidateInfo.absoluteFilePath();
    const QString rootPath = QFileInfo(root.absolutePath()).canonicalFilePath().isEmpty()
        ? root.absolutePath() : QFileInfo(root.absolutePath()).canonicalFilePath();
    const QString resolvedCandidate = candidateInfo.exists() &&
                                      !candidateInfo.canonicalFilePath().isEmpty()
        ? candidateInfo.canonicalFilePath() : candidate;
    const QString relative = QDir(rootPath).relativeFilePath(resolvedCandidate);
    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
        relative.startsWith(QStringLiteral("..\\")) || QDir::isAbsolutePath(relative)) return false;
    *absolutePath = candidate;
    return true;
}

quint16 readLe16(const uchar* bytes, qsizetype offset) {
    return quint16(bytes[offset]) | (quint16(bytes[offset + 1]) << 8);
}

quint32 readLe32(const uchar* bytes, qsizetype offset) {
    return quint32(bytes[offset]) | (quint32(bytes[offset + 1]) << 8) |
           (quint32(bytes[offset + 2]) << 16) | (quint32(bytes[offset + 3]) << 24);
}

qint32 readLeS32(const uchar* bytes, qsizetype offset) {
    return static_cast<qint32>(readLe32(bytes, offset));
}

struct BinaryBmpView {
    int width = 0;
    int height = 0;
    qint64 pixelOffset = 0;
    qint64 rowStride = 0;
    bool topDown = false;
    std::array<bool, 2> white{};
};

bool inspectBinaryBmp(const uchar* bytes, qint64 size, const QSize& expected,
                      BinaryBmpView* view, QString* error) {
    if (!bytes || size < 54 || bytes[0] != 'B' || bytes[1] != 'M') {
        *error = QStringLiteral("BMP 文件头不完整或签名无效");
        return false;
    }
    const quint32 dibSize = readLe32(bytes, 14);
    const qint32 signedWidth = readLeS32(bytes, 18);
    const qint32 signedHeight = readLeS32(bytes, 22);
    const quint16 planes = readLe16(bytes, 26);
    const quint16 bits = readLe16(bytes, 28);
    const quint32 compression = readLe32(bytes, 30);
    const quint32 pixelOffset = readLe32(bytes, 10);
    if (dibSize < 40 || signedWidth <= 0 || signedHeight == 0 ||
        signedHeight == std::numeric_limits<qint32>::min() || planes != 1 ||
        bits != 1 || compression != 0) {
        *error = QStringLiteral("BMP 必须是未压缩的 1-bit Windows BMP");
        return false;
    }
    const int height = std::abs(signedHeight);
    if (signedWidth != expected.width() || height != expected.height()) {
        *error = QStringLiteral("BMP 尺寸与报告不一致");
        return false;
    }
    const quint32 colorsUsed = readLe32(bytes, 46);
    if (colorsUsed != 0 && colorsUsed != 2) {
        *error = QStringLiteral("BMP 的 1-bit 调色板无效");
        return false;
    }
    const quint64 paletteOffset = 14ull + dibSize;
    if (paletteOffset + 8 > pixelOffset || paletteOffset + 8 > static_cast<quint64>(size)) {
        *error = QStringLiteral("BMP 调色板或像素偏移无效");
        return false;
    }
    std::array<int, 2> luminance{};
    for (int index = 0; index < 2; ++index) {
        const auto* entry = bytes + paletteOffset + quint64(index) * 4;
        if (entry[0] != entry[1] || entry[1] != entry[2] ||
            (entry[2] != 0 && entry[2] != 255)) {
            *error = QStringLiteral("BMP 调色板必须由纯黑和纯白组成");
            return false;
        }
        luminance[index] = entry[2];
    }
    if (luminance[0] == luminance[1]) {
        *error = QStringLiteral("BMP 调色板缺少黑白方向信息");
        return false;
    }
    const qint64 stride = ((qint64(signedWidth) + 31) / 32) * 4;
    const quint64 required = quint64(pixelOffset) + quint64(stride) * quint64(height);
    if (required > static_cast<quint64>(size)) {
        *error = QStringLiteral("BMP 像素数据不完整");
        return false;
    }
    view->width = signedWidth;
    view->height = height;
    view->pixelOffset = pixelOffset;
    view->rowStride = stride;
    view->topDown = signedHeight < 0;
    view->white = {luminance[0] == 255, luminance[1] == 255};
    return true;
}

bool accumulateBinaryBmp(const QString& path, const QSize& expected, const QSize& grid,
                         int layer, bool invertMask, QVector<quint16>* columnLayers,
                         QImage* thumbnail, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法打开 BMP：%1").arg(file.errorString());
        return false;
    }
    const qint64 size = file.size();
    uchar* bytes = size > 0 ? file.map(0, size) : nullptr;
    if (!bytes) {
        *error = QStringLiteral("无法映射 BMP：%1").arg(file.errorString());
        return false;
    }
    BinaryBmpView view;
    if (!inspectBinaryBmp(bytes, size, expected, &view, error)) {
        file.unmap(bytes);
        return false;
    }
    // Nearest-centre sampling matches the other stack-preview paths while avoiding
    // a libvips document, resize pipeline and QImage allocation for every layer.
    QVector<int> sourceColumns(grid.width());
    for (int x = 0; x < grid.width(); ++x)
        sourceColumns[x] = std::min(view.width - 1,
            static_cast<int>(((qint64(x) * 2 + 1) * view.width) /
                             (2LL * grid.width())));
    for (int y = 0; y < grid.height(); ++y) {
        const int sourceY = std::min(view.height - 1,
            static_cast<int>(((qint64(y) * 2 + 1) * view.height) /
                             (2LL * grid.height())));
        const int storedY = view.topDown ? sourceY : view.height - 1 - sourceY;
        const uchar* row = bytes + view.pixelOffset + qint64(storedY) * view.rowStride;
        for (int x = 0; x < grid.width(); ++x) {
            const int sourceX = sourceColumns[x];
            const int paletteIndex = (row[sourceX / 8] >> (7 - sourceX % 8)) & 1;
            const bool active = invertMask ? !view.white[paletteIndex] : view.white[paletteIndex];
            if (active) (*columnLayers)[y * grid.width() + x] = static_cast<quint16>(layer);
        }
    }

    if (thumbnail) {
        constexpr double kMaximumThumbnailSide = 128.0;
        const double scale = std::min({1.0, kMaximumThumbnailSide / view.width,
                                      kMaximumThumbnailSide / view.height});
        const QSize thumbnailSize(std::max(1, qRound(view.width * scale)),
                                  std::max(1, qRound(view.height * scale)));
        *thumbnail = QImage(thumbnailSize, QImage::Format_Grayscale8);
        for (int y = 0; y < thumbnail->height(); ++y) {
            const int sourceY = std::min(view.height - 1,
                static_cast<int>(((qint64(y) * 2 + 1) * view.height) /
                                 (2LL * thumbnail->height())));
            const int storedY = view.topDown ? sourceY : view.height - 1 - sourceY;
            const uchar* row = bytes + view.pixelOffset + qint64(storedY) * view.rowStride;
            uchar* destination = thumbnail->scanLine(y);
            for (int x = 0; x < thumbnail->width(); ++x) {
                const int sourceX = std::min(view.width - 1,
                    static_cast<int>(((qint64(x) * 2 + 1) * view.width) /
                                     (2LL * thumbnail->width())));
                const int paletteIndex = (row[sourceX / 8] >> (7 - sourceX % 8)) & 1;
                destination[x] = view.white[paletteIndex] ? 255 : 0;
            }
        }
    }
    file.unmap(bytes);
    return true;
}

QSize previewSize(const QSize& source, qint64 perModelCellBudget) {
    // Placements share one immutable height pyramid. Repeating a model must not
    // reduce the finest level because that detail cannot be recovered by runtime LOD.
    const qint64 cellBudget = std::max<qint64>(1024, perModelCellBudget);
    const double scale = std::min({1.0,
        double(kMaximumStackPreviewSide) / source.width(),
        double(kMaximumStackPreviewSide) / source.height(),
        std::sqrt(double(cellBudget) /
                  (double(source.width()) * double(source.height())))});
    return {std::max(1, qRound(source.width() * scale)),
            std::max(1, qRound(source.height() * scale))};
}

JobModelStackPtr loadModelStackData(const QString& directoryPath,
                                    const PackageSettings& settings,
                                    qint64 perModelCellBudget,
                                    const CancellationTokenPtr& token,
                                    const JobPackageLoader::Progress& progress,
                                    qint64 modelIndex, qint64 modelCount,
                                    QVector<QImage>* thumbnails, QString* error) {
    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.isDir() || directoryInfo.isSymLink() || directoryInfo.isJunction()) {
        *error = QStringLiteral("切片目录不存在或不是安全的普通目录");
        return {};
    }
    QDir directory(directoryPath);
    const QRegularExpression numericBmp(QStringLiteral("^([1-9][0-9]*)\\.bmp$"),
                                        QRegularExpression::CaseInsensitiveOption);
    QSet<int> actualLayers;
    const QFileInfoList entries = directory.entryInfoList(
        {QStringLiteral("*.bmp")}, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks);
    for (const QFileInfo& entry : entries) {
        const auto match = numericBmp.match(entry.fileName());
        if (!match.hasMatch()) {
            *error = QStringLiteral("存在不符合 {layer}.bmp 规则的切片：%1")
                         .arg(entry.fileName());
            return {};
        }
        bool ok = false;
        const qlonglong number = match.captured(1).toLongLong(&ok);
        if (!ok || number > std::numeric_limits<int>::max() ||
            actualLayers.contains(static_cast<int>(number))) {
            *error = QStringLiteral("切片编号无效或重复");
            return {};
        }
        actualLayers.insert(static_cast<int>(number));
    }
    if (actualLayers.size() != settings.layerCount) {
        *error = QStringLiteral("切片数量应为 %1，实际为 %2")
                     .arg(settings.layerCount).arg(actualLayers.size());
        return {};
    }
    for (int layer = 1; layer <= settings.layerCount; ++layer) {
        if (!actualLayers.contains(layer)) {
            *error = QStringLiteral("缺少切片 %1.bmp").arg(layer);
            return {};
        }
    }

    const QSize grid = previewSize(settings.outputSize, perModelCellBudget);
    QVector<quint16> columnLayers(qsizetype(grid.width()) * grid.height(), 0);
    if (thumbnails) {
        thumbnails->clear();
        thumbnails->reserve(settings.layerCount);
    }
    for (int layer = 1; layer <= settings.layerCount; ++layer) {
        if (token && token->isCancelled()) return {};
        const QString imagePath = directory.filePath(QString::number(layer) + QStringLiteral(".bmp"));
        const QFileInfo imageInfo(imagePath);
        if (!imageInfo.isFile() || imageInfo.isSymLink() || imageInfo.isJunction()) {
            *error = QStringLiteral("切片 %1.bmp 不是安全的普通文件").arg(layer);
            return {};
        }
        QString imageError;
        QImage thumbnail;
        if (!accumulateBinaryBmp(imagePath, settings.outputSize, grid, layer,
                                 settings.invertMask, &columnLayers,
                                 thumbnails ? &thumbnail : nullptr, &imageError)) {
            *error = QStringLiteral("无法读取 %1.bmp：%2").arg(layer).arg(imageError);
            return {};
        }
        if (thumbnails) thumbnails->push_back(std::move(thumbnail));
        if (progress) {
            const qint64 total = std::max<qint64>(1, modelCount * settings.layerCount);
            progress({QStringLiteral("加载分层模型 %1/%2").arg(modelIndex + 1).arg(modelCount),
                      modelIndex * settings.layerCount + layer, total});
        }
    }

    MaskStackPreviewData preview;
    preview.gridSize = grid;
    preview.requestedOutputSize = settings.outputSize;
    preview.physicalSize = {settings.outputSize.width() * settings.pixelScale,
                            settings.outputSize.height() * settings.pixelScale};
    preview.columnLayers = std::move(columnLayers);
    preview.layerHeight = settings.layerHeight;
    preview.layerCount = settings.layerCount;
    preview.downsampled = grid != settings.outputSize;
    preview.actualMaximumHeight = settings.layerCount * settings.layerHeight;
    const auto lodLevels = JobStackLod::buildPyramid(grid, preview.columnLayers, token);
    if (lodLevels.empty() || (token && token->isCancelled())) return {};
    MaskStackPreviewData coarse = preview;
    coarse.gridSize = lodLevels.front().gridSize;
    coarse.columnLayers = QVector<quint16>(lodLevels.front().columnLayers.cbegin(),
                                           lodLevels.front().columnLayers.cend());
    coarse.solidGeometry = MaskStackMeshBuilder::buildSolid(coarse, token, {});
    if (!coarse.solidGeometry || (token && token->isCancelled())) return {};

    auto result = std::make_shared<JobModelStack>();
    result->sourceSize = settings.outputSize;
    result->gridSize = grid;
    result->physicalSize = preview.physicalSize;
    result->layerHeight = settings.layerHeight;
    result->layerCount = settings.layerCount;
    result->downsampled = preview.downsampled;
    result->lodLevels = lodLevels;
    result->solidGeometry = coarse.solidGeometry;
    return result;
}

QString normalizedModelPath(QString path) {
    path = QDir::cleanPath(path.trimmed());
    return path.replace(QLatin1Char('\\'), QLatin1Char('/')).toCaseFolded();
}

void assignPathColors(JobDocument* document) {
    if (!document) return;
    std::set<QString> paths;
    for (const JobModel& model : document->models) paths.insert(normalizedModelPath(model.path));
    std::map<QString, QColor> colors;
    int index = 0;
    for (const QString& path : paths) {
        colors.emplace(path, QColor::fromHsv((203 + index * 137) % 360, 190, 235));
        ++index;
    }
    for (JobPoint& point : document->points) {
        if (point.placeId < 0 || point.placeId >= static_cast<qint64>(document->models.size()))
            continue;
        point.groupColor = colors.at(normalizedModelPath(
            document->models[static_cast<std::size_t>(point.placeId)].path));
    }
}
}

JobDocumentPtr JobPackageLoader::load(const QString& path, const CancellationTokenPtr& token,
                                      const Progress& progress, QString* error) {
    JobDocumentPtr document = JobFileLoader::load(path, token, progress, error);
    if (!document || (token && token->isCancelled())) return {};
    assignPathColors(document.get());
    document->sceneBounds = document->bounds;
    return document;
}

JobPackageLoader::StackLoadResult JobPackageLoader::loadStack(
    const JobDocumentPtr& document, qint64 placementId, quint64 requestGeneration,
    const CancellationTokenPtr& token, const Progress& progress, QString* error) {
    StackLoadResult result;
    result.requestGeneration = requestGeneration;
    result.placementId = placementId;
    if (error) error->clear();
    if (!document || document->source == JobDocumentSource::GeneratedPreview ||
        document->sourcePath.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("当前 Job 是临时预览，没有可读取的磁盘切片目录。");
        return result;
    }
    const auto point = std::find_if(document->points.cbegin(), document->points.cend(),
        [placementId](const JobPoint& value) { return value.placementId == placementId; });
    if (point == document->points.cend()) {
        if (error) *error = QStringLiteral("找不到编号为 %1 的坐标点。").arg(placementId + 1);
        return result;
    }
    if (point->placeId < 0 || point->placeId >= static_cast<qint64>(document->models.size())) {
        if (error) *error = QStringLiteral("坐标点引用了无效的模型编号。");
        return result;
    }
    const JobModel& model = document->models[static_cast<std::size_t>(point->placeId)];
    result.modelPath = model.path;
    const QDir root(QFileInfo(document->sourcePath).absolutePath());
    const QString reportPath = root.filePath(QStringLiteral("export-report.json"));
    const QFileInfo reportInfo(reportPath);
    PackageSettings settings;
    QString reportError;
    if (!reportInfo.exists()) {
        if (error) *error = QStringLiteral("job.json 同级目录缺少 export-report.json。");
        return result;
    }
    if (!reportInfo.isFile() || reportInfo.isSymLink() || reportInfo.isJunction() ||
        !readPackageSettings(reportPath, &settings, &reportError)) {
        if (error) *error = reportError.isEmpty()
            ? QStringLiteral("export-report.json 不是安全的普通文件") : reportError;
        return result;
    }
    QString directoryPath;
    if (!isInsideRoot(root, model.path, &directoryPath)) {
        if (error) *error = QStringLiteral("模型 path 不是 job.json 目录内的安全相对路径。");
        return result;
    }
    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.exists() || !directoryInfo.isDir() || directoryInfo.isSymLink() ||
        directoryInfo.isJunction()) {
        if (error) *error = QStringLiteral("模型切片目录不存在或不是安全目录：%1").arg(model.path);
        return result;
    }
    try {
        QString stackError;
        const JobModelStackPtr stack = loadModelStackData(
            directoryPath, settings, kTargetRenderedCellInstances, token, progress, 0, 1,
            &result.thumbnails, &stackError);
        if (!stack || (token && token->isCancelled())) {
            if (error && !(token && token->isCancelled()))
                *error = stackError.isEmpty() ? QStringLiteral("无法生成模型堆叠预览。")
                                              : stackError;
            return result;
        }
        auto preview = std::make_shared<MaskStackPreviewData>();
        preview->requestedOutputSize = stack->sourceSize;
        preview->physicalSize = stack->physicalSize;
        preview->layerHeight = stack->layerHeight;
        preview->layerCount = stack->layerCount;
        preview->downsampled = stack->downsampled;
        if (!stack->lodLevels.empty()) {
            const JobStackHeightLevel& level = stack->lodLevels.back();
            preview->gridSize = level.gridSize;
            preview->columnLayers = QVector<quint16>(level.columnLayers.cbegin(),
                                                      level.columnLayers.cend());
        }
        if (preview->gridSize.isEmpty() || preview->columnLayers.isEmpty()) {
            if (error) *error = QStringLiteral("模型切片没有可渲染的数据。");
            return result;
        }
        const auto maximum = std::max_element(preview->columnLayers.cbegin(),
                                              preview->columnLayers.cend());
        preview->actualMaximumHeight = maximum == preview->columnLayers.cend()
            ? 0.0 : *maximum * preview->layerHeight;
        preview->blocks.push_back({QRectF(QPointF(0, 0), preview->physicalSize), placementId});
        preview->solidGeometry = MaskStackMeshBuilder::buildSolid(*preview, token, progress);
        if (!preview->solidGeometry || (token && token->isCancelled())) {
            if (error && !(token && token->isCancelled()))
                *error = QStringLiteral("无法生成连续实体堆叠。");
            return result;
        }
        preview->explodedGeometry = MaskStackMeshBuilder::buildExploded(
            *preview, token, progress, kTargetExplodedQuadInstances);
        if (!preview->explodedGeometry || (token && token->isCancelled())) {
            if (error && !(token && token->isCancelled()))
                *error = QStringLiteral("无法生成分层爆炸堆叠。");
            return result;
        }
        result.preview = std::move(preview);
        return result;
    } catch (const std::bad_alloc&) {
        if (error) *error = QStringLiteral("模型切片过大，生成堆叠预览时内存不足。");
        return result;
    }
}

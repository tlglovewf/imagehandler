#include "io/JobPackageLoader.h"

#include "domain/MaskSliceModels.h"
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

struct CachedStack {
    JobModelStackPtr stack;
    QString error;
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

bool anyModelDirectoryExists(const JobDocument& document, const QDir& root) {
    for (const JobModel& model : document.models) {
        QString path;
        if (!isInsideRoot(root, model.path, &path)) continue;
        const QFileInfo info(path);
        if (info.isDir()) return true;
    }
    return false;
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
                         QString* error) {
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
    file.unmap(bytes);
    return true;
}

QSize previewSize(const QSize& source, qint64 instanceCount, qint64 perModelCellBudget) {
    const qint64 cellBudget = std::max<qint64>(1024,
        perModelCellBudget / std::max<qint64>(1, instanceCount));
    const double scale = std::min({1.0,
        double(kMaximumStackPreviewSide) / source.width(),
        double(kMaximumStackPreviewSide) / source.height(),
        std::sqrt(double(cellBudget) /
                  (double(source.width()) * double(source.height())))});
    return {std::max(1, qRound(source.width() * scale)),
            std::max(1, qRound(source.height() * scale))};
}

JobModelStackPtr loadStack(const QString& directoryPath, const PackageSettings& settings,
                           qint64 instanceCount, qint64 perModelCellBudget,
                           const CancellationTokenPtr& token,
                           const JobPackageLoader::Progress& progress, qint64 modelIndex,
                           qint64 modelCount, QString* error) {
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

    const QSize grid = previewSize(settings.outputSize, instanceCount, perModelCellBudget);
    QVector<quint16> columnLayers(qsizetype(grid.width()) * grid.height(), 0);
    for (int layer = 1; layer <= settings.layerCount; ++layer) {
        if (token && token->isCancelled()) return {};
        const QString imagePath = directory.filePath(QString::number(layer) + QStringLiteral(".bmp"));
        const QFileInfo imageInfo(imagePath);
        if (!imageInfo.isFile() || imageInfo.isSymLink() || imageInfo.isJunction()) {
            *error = QStringLiteral("切片 %1.bmp 不是安全的普通文件").arg(layer);
            return {};
        }
        QString imageError;
        if (!accumulateBinaryBmp(imagePath, settings.outputSize, grid, layer,
                                 settings.invertMask, &columnLayers, &imageError)) {
            *error = QStringLiteral("无法读取 %1.bmp：%2").arg(layer).arg(imageError);
            return {};
        }
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
    preview.solidGeometry = MaskStackMeshBuilder::buildSolid(preview, token, {});
    if (!preview.solidGeometry || (token && token->isCancelled())) return {};
    const std::size_t explodedBudget = std::max<std::size_t>(256,
        kTargetExplodedQuadInstances / static_cast<std::size_t>(std::max<qint64>(1, instanceCount)));
    preview.explodedGeometry = MaskStackMeshBuilder::buildExploded(
        preview, token, {}, explodedBudget);
    if (!preview.explodedGeometry || (token && token->isCancelled())) return {};

    auto result = std::make_shared<JobModelStack>();
    result->sourceSize = settings.outputSize;
    result->gridSize = grid;
    result->physicalSize = preview.physicalSize;
    result->layerHeight = settings.layerHeight;
    result->layerCount = settings.layerCount;
    result->downsampled = preview.downsampled;
    result->solidGeometry = preview.solidGeometry;
    result->explodedGeometry = preview.explodedGeometry;
    return result;
}

void rebuildSceneBounds(JobDocument* document) {
    if (!document) return;
    document->sceneBounds = {};
    for (const JobPoint& point : document->points) {
        document->sceneBounds.include(point.position);
        if (point.placeId < 0 || point.placeId >= static_cast<qint64>(document->models.size()))
            continue;
        const JobModel& model = document->models[static_cast<size_t>(point.placeId)];
        if (!model.stack) continue;
        document->sceneBounds.include({point.position.x + model.stack->physicalSize.width(),
                                       point.position.y + model.stack->physicalSize.height(),
                                       point.position.z + model.stack->layerCount *
                                                              model.stack->layerHeight});
    }
}
}

JobDocumentPtr JobPackageLoader::load(const QString& path, const CancellationTokenPtr& token,
                                      const Progress& progress, QString* error) {
    JobDocumentPtr document = JobFileLoader::load(path, token, progress, error);
    if (!document || (token && token->isCancelled())) return {};
    const QDir root(QFileInfo(path).absolutePath());
    const QString reportPath = root.filePath(QStringLiteral("export-report.json"));
    const QFileInfo reportInfo(reportPath);
    if (!reportInfo.exists()) {
        if (anyModelDirectoryExists(*document, root)) {
            document->layeredPackageDetected = true;
            document->packageWarnings << QStringLiteral(
                "检测到模型切片目录，但缺少 export-report.json；已仅显示 Job 坐标。");
            for (JobModel& model : document->models) {
                model.stackState = JobModelStackState::Invalid;
                model.stackError = QStringLiteral("缺少分层包报告");
            }
        }
        return document;
    }

    document->layeredPackageDetected = true;
    PackageSettings settings;
    QString reportError;
    if (!reportInfo.isFile() || reportInfo.isSymLink() || reportInfo.isJunction() ||
        !readPackageSettings(reportPath, &settings, &reportError)) {
        if (reportError.isEmpty()) reportError = QStringLiteral("分层包报告不是安全的普通文件");
        document->packageWarnings << reportError + QStringLiteral("；已仅显示 Job 坐标。");
        for (JobModel& model : document->models) {
            model.stackState = JobModelStackState::Invalid;
            model.stackError = reportError;
        }
        return document;
    }

    try {
        std::map<QString, CachedStack> cache;
        std::vector<qint64> modelInstances(document->models.size(), 0);
        for (const JobPoint& point : document->points)
            if (point.placeId >= 0 && point.placeId < static_cast<qint64>(modelInstances.size()))
                ++modelInstances[static_cast<std::size_t>(point.placeId)];
        std::map<QString, qint64> pathInstances;
        for (std::size_t index = 0; index < document->models.size(); ++index) {
            QString directoryPath;
            if (isInsideRoot(root, document->models[index].path, &directoryPath))
                pathInstances[QDir::cleanPath(directoryPath).toCaseFolded()] +=
                    modelInstances[index];
        }
        const qint64 perModelCellBudget = std::max<qint64>(4096,
            kTargetRenderedCellInstances /
            std::max<qint64>(1, static_cast<qint64>(pathInstances.size())));
        for (qsizetype index = 0; index < static_cast<qsizetype>(document->models.size()); ++index) {
            if (token && token->isCancelled()) return {};
            JobModel& model = document->models[static_cast<size_t>(index)];
            QString directoryPath;
            if (!isInsideRoot(root, model.path, &directoryPath)) {
                model.stackState = JobModelStackState::Invalid;
                model.stackError = QStringLiteral("模型路径不是 Job 目录内的安全相对路径");
            } else {
                const QString cacheKey = QDir::cleanPath(directoryPath).toCaseFolded();
                auto cached = cache.find(cacheKey);
                if (cached == cache.end()) {
                    CachedStack loaded;
                    loaded.stack = loadStack(directoryPath, settings,
                                             std::max<qint64>(1, pathInstances[cacheKey]),
                                             perModelCellBudget, token, progress,
                                             index, document->models.size(), &loaded.error);
                    if (token && token->isCancelled()) return {};
                    cached = cache.emplace(cacheKey, std::move(loaded)).first;
                }
                model.stack = cached->second.stack;
                model.stackError = cached->second.error;
                model.stackState = model.stack ? JobModelStackState::Loaded
                                               : JobModelStackState::Invalid;
            }
            if (model.stackState == JobModelStackState::Invalid) {
                document->packageWarnings << QStringLiteral("模型 %1（%2）：%3")
                    .arg(model.id).arg(model.path, model.stackError);
            }
            if (progress) {
                const qint64 total = std::max<qint64>(
                    1, static_cast<qint64>(document->models.size()) * settings.layerCount);
                progress({QStringLiteral("加载分层模型 %1/%2")
                              .arg(index + 1).arg(document->models.size()),
                          (index + 1) * settings.layerCount, total});
            }
        }
        rebuildSceneBounds(document.get());
        return document;
    } catch (const std::bad_alloc&) {
        if (error) *error = QStringLiteral("分层包过大，加载模型时内存不足。");
        return {};
    }
}

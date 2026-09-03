#include "imaging/LayeredMaskProcessor.h"

#include "imaging/MaskStackMeshBuilder.h"
#include "imaging/PointCloudMath.h"
#include "imaging/VipsSupport.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vips/vips.h>

namespace {
struct HeightRaster {
    QSize size;
    QSize contentSize;
    QVector<quint16> levels;
    double actualMaximumHeight = 0.0;
};

double storedMaximum(PixelKind kind) {
    return kind == PixelKind::Gray16 ? 65535.0 : 255.0;
}

quint16 quantizedLayer(double height, double layerHeight, int layerCount) {
    const double count = std::ceil(height / layerHeight - 1e-12);
    return static_cast<quint16>(std::clamp<int>(
        static_cast<int>(std::max(1.0, count)), 1, layerCount));
}

VipsImagePtr prepareFloatRegion(const LayeredMaskRequest& request, QString* error) {
    if (!request.source || !request.source->image || !request.region.isValid() ||
        request.region.x < 0 || request.region.y < 0 ||
        request.region.x + request.region.width > request.source->metadata.width ||
        request.region.y + request.region.height > request.source->metadata.height ||
        request.region.x > INT_MAX || request.region.y > INT_MAX ||
        request.region.width > INT_MAX || request.region.height > INT_MAX) {
        if (error) *error = QStringLiteral("切片区域无效或超出图像范围。");
        return {};
    }
    VipsImage* croppedRaw = nullptr;
    if (vips_crop(request.source->image.get(), &croppedRaw,
                  static_cast<int>(request.region.x), static_cast<int>(request.region.y),
                  static_cast<int>(request.region.width), static_cast<int>(request.region.height),
                  nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    const auto cropped = VipsSupport::adopt(croppedRaw);
    VipsImage* bandRaw = nullptr;
    if (cropped->Bands == 1) { bandRaw = cropped.get(); g_object_ref(bandRaw); }
    else if (vips_extract_band(cropped.get(), &bandRaw, 0, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    const auto band = VipsSupport::adopt(bandRaw);
    VipsImage* floatRaw = nullptr;
    if (vips_cast(band.get(), &floatRaw, VIPS_FORMAT_FLOAT, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    return VipsSupport::adopt(floatRaw);
}

bool buildRaster(const LayeredMaskRequest& request, HeightRaster* result,
                 const CancellationTokenPtr& token,
                 const LayeredMaskProcessor::Progress& progress, QString* error) {
    const auto image = prepareFloatRegion(request, error);
    if (!image) return false;
    result->size = {request.options.outputWidth, request.options.outputHeight};
    result->contentSize = LayeredMaskProcessor::contentSize(request);
    if (result->size.height() <= 0 || result->size.width() <= 0 ||
        qsizetype(result->size.width()) >
            std::numeric_limits<qsizetype>::max() / result->size.height()) {
        if (error) *error = QStringLiteral("标准块尺寸过大。");
        return false;
    }
    const qsizetype pixels = qsizetype(result->size.width()) * result->size.height();
    if (pixels > std::numeric_limits<qsizetype>::max() / qsizetype(sizeof(quint16))) {
        if (error) *error = QStringLiteral("标准块尺寸过大。");
        return false;
    }
    result->levels.fill(0, pixels);
    VipsRegion* source = vips_region_new(image.get());
    if (!source) { if (error) *error = VipsSupport::lastError(); return false; }
    QVector<int> columns(result->contentSize.width());
    for (int x = 0; x < columns.size(); ++x)
        columns[x] = std::min(image->Xsize - 1,
            static_cast<int>((quint64(x) * image->Xsize) / quint64(columns.size())));
    const PointCloudSettings mapping{request.options.pixelScale,
        request.options.maximumHeight, request.options.invertHeight};
    const double divisor = storedMaximum(request.source->metadata.kind);
    for (int y = 0; y < result->contentSize.height(); ++y) {
        if (token && token->isCancelled()) { g_object_unref(source); return false; }
        const int sourceY = std::min(image->Ysize - 1,
            static_cast<int>((quint64(y) * image->Ysize) /
                             quint64(result->contentSize.height())));
        VipsRect rectangle{0, sourceY, image->Xsize, 1};
        if (vips_region_prepare(source, &rectangle) != 0) {
            g_object_unref(source);
            if (error) *error = VipsSupport::lastError();
            return false;
        }
        const auto* values = reinterpret_cast<const float*>(
            VIPS_REGION_ADDR(source, 0, sourceY));
        for (int x = 0; x < result->contentSize.width(); ++x) {
            const double height = PointCloudMath::heightForRaw(values[columns[x]], divisor,
                                                                mapping);
            result->actualMaximumHeight = std::max(result->actualMaximumHeight, height);
            result->levels[y * result->size.width() + x] =
                quantizedLayer(height, request.options.layerHeight, request.layerCount);
        }
        if (progress && (y + 1 == result->contentSize.height() || y % 32 == 0))
            progress({QStringLiteral("准备标准块高度数据"), y + 1,
                      result->contentSize.height()});
    }
    g_object_unref(source);
    return true;
}

struct SourcePoint { double x = 0.0; double y = 0.0; };

SourcePoint sourcePoint(const LayeredPackageOptions& options, double outputX, double outputY) {
    if (!options.attachToCanvas) return {outputX, outputY};
    const double radians = options.rotationDegrees * std::numbers::pi / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double dx = outputX - options.canvasWidth / 2.0;
    const double dy = outputY - options.canvasHeight / 2.0;
    const double unrotatedX = cosine * dx + sine * dy;
    const double unrotatedY = -sine * dx + cosine * dy;
    return {unrotatedX / options.scaleX + options.outputWidth / 2.0,
            unrotatedY / options.scaleY + options.outputHeight / 2.0};
}

quint16 levelAt(const HeightRaster& raster, int x, int y) {
    if (x < 0 || y < 0 || x >= raster.size.width() || y >= raster.size.height()) return 0;
    return raster.levels[y * raster.size.width() + x];
}

double activeCoverage(const HeightRaster& raster, const SourcePoint& point, int layer,
                      BinaryTransformSampling sampling) {
    if (point.x < 0.0 || point.y < 0.0 || point.x >= raster.size.width() ||
        point.y >= raster.size.height()) return 0.0;
    if (sampling == BinaryTransformSampling::Nearest) {
        return levelAt(raster, static_cast<int>(std::floor(point.x)),
                       static_cast<int>(std::floor(point.y))) >= layer ? 1.0 : 0.0;
    }
    const double pixelX = point.x - 0.5;
    const double pixelY = point.y - 0.5;
    const int left = static_cast<int>(std::floor(pixelX));
    const int top = static_cast<int>(std::floor(pixelY));
    const double fx = pixelX - left;
    const double fy = pixelY - top;
    const auto active = [&](int x, int y) { return levelAt(raster, x, y) >= layer ? 1.0 : 0.0; };
    return active(left, top) * (1.0 - fx) * (1.0 - fy) +
           active(left + 1, top) * fx * (1.0 - fy) +
           active(left, top + 1) * (1.0 - fx) * fy +
           active(left + 1, top + 1) * fx * fy;
}

quint16 transformedLevel(const HeightRaster& raster, const LayeredPackageOptions& options,
                         double outputX, double outputY, int layerCount) {
    const SourcePoint point = sourcePoint(options, outputX, outputY);
    if (options.transformSampling == BinaryTransformSampling::Nearest)
        return levelAt(raster, static_cast<int>(std::floor(point.x)),
                       static_cast<int>(std::floor(point.y)));
    int low = 1, high = layerCount, result = 0;
    while (low <= high) {
        const int middle = low + (high - low) / 2;
        if (activeCoverage(raster, point, middle, options.transformSampling) >= 0.5) {
            result = middle; low = middle + 1;
        } else high = middle - 1;
    }
    return static_cast<quint16>(result);
}

bool writeHeader(QFile* file, const QSize& size, quint64 fileSize, QString* error) {
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = file->errorString();
        return false;
    }
    constexpr quint32 pixelOffset = 62;
    const quint32 stride = static_cast<quint32>(((quint64(size.width()) + 31) / 32) * 4);
    QDataStream out(file); out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("BM", 2);
    out << quint32(fileSize) << quint16(0) << quint16(0) << quint32(pixelOffset);
    out << quint32(40) << qint32(size.width()) << qint32(-size.height()) << quint16(1)
        << quint16(1) << quint32(0) << quint32(quint64(stride) * size.height())
        << qint32(2835) << qint32(2835) << quint32(2) << quint32(0);
    out << quint32(0x00000000) << quint32(0x00ffffff);
    if (out.status() != QDataStream::Ok) {
        if (error) *error = file->errorString();
        return false;
    }
    return true;
}
}

QSize LayeredMaskProcessor::outputSize(const LayeredPackageOptions& options) {
    return options.attachToCanvas ? QSize(options.canvasWidth, options.canvasHeight)
                                  : QSize(options.outputWidth, options.outputHeight);
}

QSize LayeredMaskProcessor::contentSize(const LayeredMaskRequest& request) {
    if (request.nominalSourceSize.width() <= 0 || request.nominalSourceSize.height() <= 0)
        return {};
    return {static_cast<int>(std::clamp<qint64>(qRound64(
                double(request.region.width) * request.options.outputWidth /
                request.nominalSourceSize.width()), 1, request.options.outputWidth)),
            static_cast<int>(std::clamp<qint64>(qRound64(
                double(request.region.height) * request.options.outputHeight /
                request.nominalSourceSize.height()), 1, request.options.outputHeight))};
}

bool LayeredMaskProcessor::validate(const LayeredMaskRequest& request, QString* error) {
    if (error) error->clear();
    if (!request.source || !request.source->image || !request.region.isValid() ||
        request.nominalSourceSize.width() <= 0 || request.nominalSourceSize.height() <= 0 ||
        request.options.outputWidth <= 0 || request.options.outputHeight <= 0 ||
        request.layerCount <= 0 || request.layerCount > kMaximumMaskSliceLayers ||
        !std::isfinite(request.options.scaleX) || request.options.scaleX <= 0.0 ||
        !std::isfinite(request.options.scaleY) || request.options.scaleY <= 0.0 ||
        !std::isfinite(request.options.rotationDegrees)) {
        if (error) *error = QStringLiteral("分层切片参数无效。");
        return false;
    }
    if (!request.options.attachToCanvas) return true;
    if (request.options.canvasWidth <= 0 || request.options.canvasHeight <= 0) {
        if (error) *error = QStringLiteral("导出底图尺寸必须大于 0。");
        return false;
    }
    if (request.options.outputWidth > request.options.canvasWidth ||
        request.options.outputHeight > request.options.canvasHeight) {
        if (error) *error = QStringLiteral("标准块 %1×%2 超过导出底图 %3×%4。")
            .arg(request.options.outputWidth).arg(request.options.outputHeight)
            .arg(request.options.canvasWidth).arg(request.options.canvasHeight);
        return false;
    }
    const double radians = request.options.rotationDegrees * std::numbers::pi / 180.0;
    const double transformedWidth =
        std::abs(request.options.outputWidth * request.options.scaleX * std::cos(radians)) +
        std::abs(request.options.outputHeight * request.options.scaleY * std::sin(radians));
    const double transformedHeight =
        std::abs(request.options.outputWidth * request.options.scaleX * std::sin(radians)) +
        std::abs(request.options.outputHeight * request.options.scaleY * std::cos(radians));
    if (transformedWidth > request.options.canvasWidth + 1e-9 ||
        transformedHeight > request.options.canvasHeight + 1e-9) {
        if (error) *error = QStringLiteral("缩放旋转后的外接范围 %1×%2 超过导出底图 %3×%4。")
            .arg(std::ceil(transformedWidth)).arg(std::ceil(transformedHeight))
            .arg(request.options.canvasWidth).arg(request.options.canvasHeight);
        return false;
    }
    return true;
}

quint64 LayeredMaskProcessor::bmpFileSize(const QSize& size) {
    if (size.width() <= 0 || size.height() <= 0) return 0;
    const quint64 stride = ((quint64(size.width()) + 31) / 32) * 4;
    if (stride > (std::numeric_limits<quint64>::max() - 62) / quint64(size.height())) return 0;
    return 62 + stride * quint64(size.height());
}

LayeredMaskPreview LayeredMaskProcessor::preview(
    const LayeredMaskRequest& request, const CancellationTokenPtr& token,
    const Progress& progress, QString* error) {
    LayeredMaskPreview result;
    result.requestGeneration = request.requestGeneration;
    if (!validate(request, error)) return result;
    HeightRaster raster;
    if (!buildRaster(request, &raster, token, progress, error)) return {};
    result.outputSize = outputSize(request.options);
    result.contentSize = raster.contentSize;
    result.actualMaximumHeight = raster.actualMaximumHeight;
    constexpr double kThumbnailSide = 128.0;
    constexpr double kStackSide = 512.0;
    const double thumbScale = std::min({1.0, kThumbnailSide / result.outputSize.width(),
                                        kThumbnailSide / result.outputSize.height()});
    const QSize thumbnailSize(std::max(1, qRound(result.outputSize.width() * thumbScale)),
                              std::max(1, qRound(result.outputSize.height() * thumbScale)));
    result.thumbnails.reserve(request.layerCount);
    for (int layer = 1; layer <= request.layerCount; ++layer) {
        if (token && token->isCancelled()) return {};
        QImage image(thumbnailSize, QImage::Format_Grayscale8);
        for (int y = 0; y < image.height(); ++y) {
            uchar* row = image.scanLine(y);
            const double outputY = (y + 0.5) * result.outputSize.height() / image.height();
            for (int x = 0; x < image.width(); ++x) {
                const double outputX = (x + 0.5) * result.outputSize.width() / image.width();
                const bool active = activeCoverage(raster, sourcePoint(request.options,
                    outputX, outputY), layer, request.options.transformSampling) >= 0.5;
                const bool white = request.options.invertMask ? !active : active;
                row[x] = white ? 255 : 0;
            }
        }
        result.thumbnails.push_back(std::move(image));
        if (progress) progress({QStringLiteral("生成切片预览"), layer, request.layerCount});
    }
    const double stackScale = std::min({1.0, kStackSide / result.outputSize.width(),
                                        kStackSide / result.outputSize.height()});
    auto stack = std::make_shared<MaskStackPreviewData>();
    stack->requestedOutputSize = result.outputSize;
    stack->gridSize = {std::max(1, qRound(result.outputSize.width() * stackScale)),
                       std::max(1, qRound(result.outputSize.height() * stackScale))};
    stack->physicalSize = {result.outputSize.width() * request.options.pixelScale,
                           result.outputSize.height() * request.options.pixelScale};
    stack->layerHeight = request.options.layerHeight;
    stack->layerCount = request.layerCount;
    stack->downsampled = stack->gridSize != result.outputSize;
    stack->actualMaximumHeight = result.actualMaximumHeight;
    stack->columnLayers.resize(qsizetype(stack->gridSize.width()) * stack->gridSize.height());
    for (int y = 0; y < stack->gridSize.height(); ++y) {
        if (token && token->isCancelled()) return {};
        const double outputY = (y + 0.5) * result.outputSize.height() / stack->gridSize.height();
        for (int x = 0; x < stack->gridSize.width(); ++x) {
            const double outputX = (x + 0.5) * result.outputSize.width() / stack->gridSize.width();
            stack->columnLayers[y * stack->gridSize.width() + x] =
                transformedLevel(raster, request.options, outputX, outputY, request.layerCount);
        }
    }
    stack->solidGeometry = MaskStackMeshBuilder::buildSolid(*stack, token, progress);
    if (!stack->solidGeometry || (token && token->isCancelled())) return {};
    stack->explodedGeometry = MaskStackMeshBuilder::buildExploded(*stack, token, progress);
    if (!stack->explodedGeometry || (token && token->isCancelled())) return {};
    result.stackPreview = stack;
    return result;
}

bool LayeredMaskProcessor::exportAll(
    const QString& outputDirectory, const LayeredMaskRequest& request,
    const CancellationTokenPtr& token, const Progress& progress, QString* error) {
    if (!validate(request, error) || !QDir(outputDirectory).exists()) return false;
    HeightRaster raster;
    if (!buildRaster(request, &raster, token, progress, error)) return false;
    const QSize finalSize = outputSize(request.options);
    const quint64 fileSize = bmpFileSize(finalSize);
    if (!fileSize || fileSize > std::numeric_limits<quint32>::max()) {
        if (error) *error = QStringLiteral("单个掩模超过经典 BMP 的 4 GB 限制。");
        return false;
    }
    const quint64 stride = ((quint64(finalSize.width()) + 31) / 32) * 4;
    QByteArray packed(static_cast<qsizetype>(stride), '\0');
    const qint64 totalRows = qint64(request.layerCount) * finalSize.height();
    qint64 completedRows = 0;
    for (int layer = 1; layer <= request.layerCount; ++layer) {
        if (token && token->isCancelled()) return false;
        QFile file(QDir(outputDirectory).filePath(QString::number(layer) + QStringLiteral(".bmp")));
        if (!writeHeader(&file, finalSize, fileSize, error)) return false;
        for (int y = 0; y < finalSize.height(); ++y) {
            if (token && token->isCancelled()) return false;
            packed.fill('\0');
            for (int x = 0; x < finalSize.width(); ++x) {
                const bool active = activeCoverage(raster, sourcePoint(request.options,
                    x + 0.5, y + 0.5), layer, request.options.transformSampling) >= 0.5;
                const bool white = request.options.invertMask ? !active : active;
                if (white) packed[x / 8] = char(uchar(packed[x / 8]) |
                                                uchar(0x80u >> (x % 8)));
            }
            if (file.write(packed) != packed.size()) {
                if (error) *error = file.errorString();
                return false;
            }
            ++completedRows;
            if (progress && (completedRows == totalRows || completedRows % 32 == 0))
                progress({QStringLiteral("导出第 %1/%2 层").arg(layer).arg(request.layerCount),
                          completedRows, totalRows});
        }
        if (!file.flush()) { if (error) *error = file.errorString(); return false; }
    }
    return true;
}

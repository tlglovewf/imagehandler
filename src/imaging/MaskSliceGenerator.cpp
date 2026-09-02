#include "imaging/MaskSliceGenerator.h"

#include "imaging/ImageLoader.h"
#include "imaging/PointCloudMath.h"
#include "imaging/MaskStackMeshBuilder.h"
#include "imaging/VipsSupport.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vips/vips.h>

namespace {
VipsImagePtr prepareFloatRegion(const MaskSliceRequest& request, QString* error) {
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
    if (cropped->Bands == 1) {
        bandRaw = cropped.get();
        g_object_ref(bandRaw);
    } else if (vips_extract_band(cropped.get(), &bandRaw, 0, nullptr) != 0) {
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

double storedMaximum(PixelKind kind) {
    return kind == PixelKind::Gray16 ? 65535.0 : 255.0;
}

QSize previewSizeFor(const MaskSliceRequest& request) {
    constexpr double kMaximumSide = 128.0;
    const double width = std::max(1, request.settings.outputWidth);
    const double height = std::max(1, request.settings.outputHeight);
    const double scale = std::min({1.0, kMaximumSide / width, kMaximumSide / height});
    return {std::max(1, static_cast<int>(std::lround(width * scale))),
            std::max(1, static_cast<int>(std::lround(height * scale)))};
}

QSize stackSizeFor(const MaskSliceRequest& request) {
    constexpr double kMaximumSide = 512.0;
    const double width = std::max(1, request.settings.outputWidth);
    const double height = std::max(1, request.settings.outputHeight);
    const double scale = std::min({1.0, kMaximumSide / width, kMaximumSide / height});
    return {std::max(1, static_cast<int>(std::lround(width * scale))),
            std::max(1, static_cast<int>(std::lround(height * scale)))};
}

quint16 quantizedLayer(double height, double layerHeight, int layerCount) {
    if (!(height > 0.0) || !(layerHeight > 0.0) || layerCount <= 0) return 0;
    const double count = std::ceil(height / layerHeight - 1e-12);
    return static_cast<quint16>(std::clamp<int>(
        static_cast<int>(std::max(1.0, count)), 1, layerCount));
}
}

int MaskSliceGenerator::layerCount(double actualMaximumHeight,
                                   double configuredMaximumHeight,
                                   double layerHeight, QString* error) {
    if (!std::isfinite(layerHeight) || layerHeight <= 0.0 ||
        !std::isfinite(actualMaximumHeight) || actualMaximumHeight < 0.0 ||
        !std::isfinite(configuredMaximumHeight) || configuredMaximumHeight <= 0.0) {
        if (error) *error = QStringLiteral("层切高和高度映射参数必须是有效的正数。");
        return 0;
    }
    const double height = actualMaximumHeight > std::numeric_limits<double>::epsilon()
                              ? actualMaximumHeight : configuredMaximumHeight;
    const double layers = std::ceil(height / layerHeight - 1e-12);
    if (!std::isfinite(layers) || layers < 1.0 || layers > kMaximumMaskSliceLayers) {
        if (error) {
            *error = layers > kMaximumMaskSliceLayers
                ? QStringLiteral("掩模数量超过 %1 张，请增大层切高。")
                      .arg(kMaximumMaskSliceLayers)
                : QStringLiteral("无法根据当前层切高计算掩模数量。");
        }
        return 0;
    }
    return static_cast<int>(layers);
}

MaskSliceAnalysis MaskSliceGenerator::analyze(
    const MaskSliceRequest& request, const CancellationTokenPtr& token,
    const Progress& progress, QString* error) {
    MaskSliceAnalysis result;
    result.requestGeneration = request.requestGeneration;
    if (!request.source || !ImageLoader::isProcessable(request.source->metadata) ||
        request.settings.outputWidth <= 0 || request.settings.outputHeight <= 0 ||
        !std::isfinite(request.mapping.pixelScale) || request.mapping.pixelScale <= 0.0 ||
        !std::isfinite(request.mapping.maximumHeight) || request.mapping.maximumHeight <= 0.0) {
        if (error) *error = QStringLiteral("切片请求或像素物理映射参数无效。");
        return result;
    }
    const auto floatImage = prepareFloatRegion(request, error);
    if (!floatImage || (token && token->isCancelled())) return {};

    result.sourceSize = {floatImage->Xsize, floatImage->Ysize};
    const double divisor = storedMaximum(request.source->metadata.kind);
    VipsRegion* scanRegion = vips_region_new(floatImage.get());
    if (!scanRegion) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    constexpr int kStripRows = 64;
    if (progress) progress({QStringLiteral("扫描单元最高点"), 0, floatImage->Ysize});
    for (int top = 0; top < floatImage->Ysize; top += kStripRows) {
        if (token && token->isCancelled()) { g_object_unref(scanRegion); return {}; }
        const int rows = std::min(kStripRows, floatImage->Ysize - top);
        VipsRect rectangle{0, top, floatImage->Xsize, rows};
        if (vips_region_prepare(scanRegion, &rectangle) != 0) {
            g_object_unref(scanRegion);
            if (error) *error = VipsSupport::lastError();
            return {};
        }
        for (int y = top; y < top + rows; ++y) {
            const auto* row = reinterpret_cast<const float*>(VIPS_REGION_ADDR(scanRegion, 0, y));
            for (int x = 0; x < floatImage->Xsize; ++x) {
                result.actualMaximumHeight = std::max(
                    result.actualMaximumHeight,
                    PointCloudMath::heightForRaw(row[x], divisor, request.mapping));
            }
        }
        if (progress) progress({QStringLiteral("扫描单元最高点"),
                                std::min(top + rows, floatImage->Ysize), floatImage->Ysize});
    }
    g_object_unref(scanRegion);

    result.layerCount = layerCount(result.actualMaximumHeight,
                                   request.mapping.maximumHeight,
                                   request.settings.layerHeight, error);
    if (result.layerCount <= 0 || (token && token->isCancelled())) return {};

    if (request.generateStackPreview) {
    auto stackPreview = std::make_shared<MaskStackPreviewData>();
    stackPreview->requestedOutputSize = {request.settings.outputWidth,
                                         request.settings.outputHeight};
    stackPreview->gridSize = stackSizeFor(request);
    stackPreview->physicalSize = {
        request.region.width * request.mapping.pixelScale,
        request.region.height * request.mapping.pixelScale};
    stackPreview->layerHeight = request.settings.layerHeight;
    stackPreview->layerCount = result.layerCount;
    stackPreview->actualMaximumHeight = result.actualMaximumHeight;
    stackPreview->downsampled = stackPreview->gridSize != stackPreview->requestedOutputSize;

    VipsImage* stackRaw = nullptr;
    const double stackScaleX = double(stackPreview->gridSize.width()) / floatImage->Xsize;
    const double stackScaleY = double(stackPreview->gridSize.height()) / floatImage->Ysize;
    if (vips_resize(floatImage.get(), &stackRaw, stackScaleX,
                    "vscale", stackScaleY, "kernel", VIPS_KERNEL_NEAREST,
                    nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    const auto stackImage = VipsSupport::adopt(stackRaw);
    stackPreview->gridSize = {stackImage->Xsize, stackImage->Ysize};
    stackPreview->downsampled = stackPreview->gridSize != stackPreview->requestedOutputSize;
    size_t stackBytes = 0;
    void* stackMemory = vips_image_write_to_memory(stackImage.get(), &stackBytes);
    if (!stackMemory) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    const auto* stackValues = static_cast<const float*>(stackMemory);
    const qsizetype stackPixels = qsizetype(stackPreview->gridSize.width()) *
                                  stackPreview->gridSize.height();
    stackPreview->columnLayers.resize(stackPixels);
    if (progress) progress({QStringLiteral("生成三维层高数据"), 0, stackPixels});
    for (qsizetype index = 0; index < stackPixels; ++index) {
        if ((index & 0x3fff) == 0 && token && token->isCancelled()) {
            g_free(stackMemory);
            return {};
        }
        const double height = PointCloudMath::heightForRaw(
            stackValues[index], divisor, request.mapping);
        stackPreview->columnLayers[index] = quantizedLayer(
            height, request.settings.layerHeight, result.layerCount);
    }
    g_free(stackMemory);
    if (progress) progress({QStringLiteral("生成三维层高数据"), stackPixels, stackPixels});
    stackPreview->solidGeometry = MaskStackMeshBuilder::buildSolid(
        *stackPreview, token, progress);
    if (!stackPreview->solidGeometry || (token && token->isCancelled())) return {};
    stackPreview->explodedGeometry = MaskStackMeshBuilder::buildExploded(
        *stackPreview, token, progress);
    if (!stackPreview->explodedGeometry || (token && token->isCancelled())) return {};
    result.stackPreview = stackPreview;
    }

    result.previewSize = previewSizeFor(request);
    VipsImage* resizedRaw = nullptr;
    const double horizontalScale = double(result.previewSize.width()) / floatImage->Xsize;
    const double verticalScale = double(result.previewSize.height()) / floatImage->Ysize;
    if (vips_resize(floatImage.get(), &resizedRaw, horizontalScale,
                    "vscale", verticalScale, "kernel", VIPS_KERNEL_NEAREST, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    const auto resized = VipsSupport::adopt(resizedRaw);
    result.previewSize = {resized->Xsize, resized->Ysize};
    size_t byteCount = 0;
    void* memory = vips_image_write_to_memory(resized.get(), &byteCount);
    if (!memory) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    const auto* values = static_cast<const float*>(memory);
    const qsizetype pixelCount = qsizetype(result.previewSize.width()) * result.previewSize.height();
    result.thumbnails.reserve(result.layerCount);
    if (progress) progress({QStringLiteral("生成掩模缩略图"), 0, result.layerCount});
    for (int layer = 0; layer < result.layerCount; ++layer) {
        if (token && token->isCancelled()) {
            g_free(memory);
            return {};
        }
        QImage thumbnail(result.previewSize, QImage::Format_Grayscale8);
        const double lowerHeight = layer * request.settings.layerHeight;
        for (int y = 0; y < thumbnail.height(); ++y) {
            uchar* destination = thumbnail.scanLine(y);
            for (int x = 0; x < thumbnail.width(); ++x) {
                const qsizetype index = qsizetype(y) * thumbnail.width() + x;
                const bool active = PointCloudMath::heightForRaw(
                                        values[index], divisor, request.mapping) > lowerHeight;
                const bool white = request.settings.invertMask ? !active : active;
                destination[x] = white ? 255 : 0;
            }
        }
        result.thumbnails.push_back(std::move(thumbnail));
        if (progress) progress({QStringLiteral("生成掩模缩略图"), layer + 1,
                                result.layerCount});
    }
    g_free(memory);
    return result;
}

MaskStackGroupAnalysis MaskSliceGenerator::analyzeStackGroup(
    const MaskStackGroupRequest& request, const CancellationTokenPtr& token,
    const Progress& progress, QString* error) {
    MaskStackGroupAnalysis result;
    result.requestGeneration = request.requestGeneration;
    if (!request.source || !ImageLoader::isProcessable(request.source->metadata) ||
        request.elements.isEmpty() || !std::isfinite(request.layerHeight) ||
        request.layerHeight <= 0.0 || !std::isfinite(request.outputScaleX) ||
        request.outputScaleX <= 0.0 || !std::isfinite(request.outputScaleY) ||
        request.outputScaleY <= 0.0 || !std::isfinite(request.mapping.pixelScale) ||
        request.mapping.pixelScale <= 0.0 || !std::isfinite(request.mapping.maximumHeight) ||
        request.mapping.maximumHeight <= 0.0) {
        if (error) *error = QStringLiteral("多块三维预览请求无效。");
        return result;
    }

    qint64 left = std::numeric_limits<qint64>::max();
    qint64 top = std::numeric_limits<qint64>::max();
    qint64 right = 0;
    qint64 bottom = 0;
    for (const MaskSliceElement& element : request.elements) {
        if (!element.region.isValid() || element.region.x < 0 || element.region.y < 0 ||
            element.region.x + element.region.width > request.source->metadata.width ||
            element.region.y + element.region.height > request.source->metadata.height) {
            if (error) *error = QStringLiteral("多选块区域无效或超出源图范围。");
            return result;
        }
        left = std::min(left, element.region.x);
        top = std::min(top, element.region.y);
        right = std::max(right, element.region.x + element.region.width);
        bottom = std::max(bottom, element.region.y + element.region.height);
    }
    const qint64 boundingWidth = right - left;
    const qint64 boundingHeight = bottom - top;
    if (boundingWidth <= 0 || boundingHeight <= 0 || boundingWidth > INT_MAX ||
        boundingHeight > INT_MAX || left > INT_MAX || top > INT_MAX) {
        if (error) *error = QStringLiteral("多选块包围区域过大。");
        return result;
    }

    auto preview = std::make_shared<MaskStackPreviewData>();
    const auto requestedDimension = [](qint64 sourcePixels, double scale) {
        const long double value = std::round(static_cast<long double>(sourcePixels) * scale);
        return static_cast<int>(std::clamp<long double>(value, 1.0L, INT_MAX));
    };
    preview->requestedOutputSize = {
        requestedDimension(boundingWidth, request.outputScaleX),
        requestedDimension(boundingHeight, request.outputScaleY)};
    constexpr double kMaximumSide = 512.0;
    const double previewScale = std::min({1.0,
        kMaximumSide / preview->requestedOutputSize.width(),
        kMaximumSide / preview->requestedOutputSize.height()});
    preview->gridSize = {
        std::max(1, static_cast<int>(std::lround(
            preview->requestedOutputSize.width() * previewScale))),
        std::max(1, static_cast<int>(std::lround(
            preview->requestedOutputSize.height() * previewScale)))};
    preview->physicalSize = {boundingWidth * request.mapping.pixelScale,
                             boundingHeight * request.mapping.pixelScale};
    preview->layerHeight = request.layerHeight;
    preview->downsampled = preview->gridSize != preview->requestedOutputSize;
    preview->blocks.reserve(request.elements.size());
    for (const MaskSliceElement& element : request.elements) {
        preview->blocks.push_back({
            QRectF((element.region.x - left) * request.mapping.pixelScale,
                   (bottom - element.region.y - element.region.height) * request.mapping.pixelScale,
                   element.region.width * request.mapping.pixelScale,
                   element.region.height * request.mapping.pixelScale),
            element.traversalIndex});
    }

    const double divisor = storedMaximum(request.source->metadata.kind);
    result.elements.reserve(request.elements.size());
    if (progress) progress({QStringLiteral("扫描多选块最高点"), 0,
                            request.elements.size()});
    for (qsizetype index = 0; index < request.elements.size(); ++index) {
        if (token && token->isCancelled()) return {};
        MaskSliceRequest single{request.source, request.elements[index].region,
                                request.mapping, {request.layerHeight, 1, 1, false}};
        const auto image = prepareFloatRegion(single, error);
        if (!image) return result;
        double rawExtreme = 0.0;
        const int status = request.mapping.invertHeight
            ? vips_min(image.get(), &rawExtreme, nullptr)
            : vips_max(image.get(), &rawExtreme, nullptr);
        if (status != 0) {
            if (error) *error = VipsSupport::lastError();
            return result;
        }
        const double elementMaximumHeight = PointCloudMath::heightForRaw(
            rawExtreme, divisor, request.mapping);
        QString layerError;
        const int elementLayerCount = layerCount(
            elementMaximumHeight, request.mapping.maximumHeight,
            request.layerHeight, &layerError);
        if (elementLayerCount <= 0) {
            if (error) *error = layerError;
            return result;
        }
        result.elements.push_back({request.elements[index].traversalIndex,
                                   elementMaximumHeight, elementLayerCount});
        result.commonLayerCount = std::max(result.commonLayerCount, elementLayerCount);
        preview->actualMaximumHeight = std::max(preview->actualMaximumHeight,
                                                elementMaximumHeight);
        if (progress) progress({QStringLiteral("扫描多选块最高点"), index + 1,
                                request.elements.size()});
    }
    preview->layerCount = result.commonLayerCount;
    if (preview->layerCount <= 0 || (token && token->isCancelled())) return result;

    MaskSliceRequest boundingRequest{request.source, {left, top, boundingWidth, boundingHeight},
                                     request.mapping, {request.layerHeight, 1, 1, false}};
    const auto boundingImage = prepareFloatRegion(boundingRequest, error);
    if (!boundingImage) return result;
    VipsImage* resizedRaw = nullptr;
    if (vips_resize(boundingImage.get(), &resizedRaw,
                    double(preview->gridSize.width()) / boundingImage->Xsize,
                    "vscale", double(preview->gridSize.height()) / boundingImage->Ysize,
                    "kernel", VIPS_KERNEL_NEAREST, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return result;
    }
    const auto resized = VipsSupport::adopt(resizedRaw);
    preview->gridSize = {resized->Xsize, resized->Ysize};
    const qsizetype pixelCount = qsizetype(resized->Xsize) * resized->Ysize;
    QVector<quint8> selected(pixelCount, 0);
    const double sourcePerPreviewX = double(boundingWidth) / resized->Xsize;
    const double sourcePerPreviewY = double(boundingHeight) / resized->Ysize;
    for (const MaskSliceElement& element : request.elements) {
        const int xBegin = std::max(0, static_cast<int>(std::floor(
            double(element.region.x - left) / boundingWidth * resized->Xsize)));
        const int xEnd = std::min(resized->Xsize, static_cast<int>(std::ceil(
            double(element.region.x + element.region.width - left) / boundingWidth * resized->Xsize)));
        const int yBegin = std::max(0, static_cast<int>(std::floor(
            double(element.region.y - top) / boundingHeight * resized->Ysize)));
        const int yEnd = std::min(resized->Ysize, static_cast<int>(std::ceil(
            double(element.region.y + element.region.height - top) / boundingHeight * resized->Ysize)));
        for (int y = yBegin; y < yEnd; ++y) {
            const double sourceY = top + (y + 0.5) * sourcePerPreviewY;
            if (sourceY < element.region.y ||
                sourceY >= element.region.y + element.region.height) continue;
            for (int x = xBegin; x < xEnd; ++x) {
                const double sourceX = left + (x + 0.5) * sourcePerPreviewX;
                if (sourceX >= element.region.x &&
                    sourceX < element.region.x + element.region.width)
                    selected[qsizetype(y) * resized->Xsize + x] = 1;
            }
        }
    }

    size_t byteCount = 0;
    void* memory = vips_image_write_to_memory(resized.get(), &byteCount);
    if (!memory) {
        if (error) *error = VipsSupport::lastError();
        return result;
    }
    const auto* values = static_cast<const float*>(memory);
    preview->columnLayers.resize(pixelCount);
    if (progress) progress({QStringLiteral("生成多块三维层高数据"), 0, pixelCount});
    for (qsizetype index = 0; index < pixelCount; ++index) {
        if ((index & 0x3fff) == 0 && token && token->isCancelled()) {
            g_free(memory);
            return {};
        }
        if (!selected[index]) continue;
        preview->columnLayers[index] = quantizedLayer(
            PointCloudMath::heightForRaw(values[index], divisor, request.mapping),
            request.layerHeight, preview->layerCount);
    }
    g_free(memory);
    if (progress) progress({QStringLiteral("生成多块三维层高数据"), pixelCount, pixelCount});

    preview->solidGeometry = MaskStackMeshBuilder::buildSolid(*preview, token, progress);
    if (!preview->solidGeometry || (token && token->isCancelled())) return {};
    preview->explodedGeometry = MaskStackMeshBuilder::buildExploded(*preview, token, progress);
    if (!preview->explodedGeometry || (token && token->isCancelled())) return {};
    result.preview = preview;
    return result;
}

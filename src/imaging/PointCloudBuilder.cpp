#include "imaging/PointCloudBuilder.h"

#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vips/vips.h>

namespace {
qint64 divideRoundUp(qint64 value, qint64 divisor) {
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

float maximumStoredValue(PixelKind kind) {
    return kind == PixelKind::Gray16 ? 65535.0F : 255.0F;
}
}

int PointCloudBuilder::rootLevelFor(const ImageMetadata& metadata) {
    if (metadata.width <= 0 || metadata.height <= 0) return 0;
    int level = 0;
    while (level < 30) {
        const qint64 step = qint64(1) << level;
        const qint64 columns = divideRoundUp(metadata.width, step);
        const qint64 rows = divideRoundUp(metadata.height, step);
        if (columns <= kPointCloudRootPointBudget / std::max<qint64>(1, rows)) break;
        ++level;
    }
    return level;
}

PointCloudDocumentPtr PointCloudBuilder::build(
    const ImageDocumentPtr& source, const PointCloudSettings& settings, quint64 generation,
    const CancellationTokenPtr& token, const std::function<void(const TaskProgress&)>& progress,
    QString* error) {
    if (!source || !source->image || !ImageLoader::isProcessable(source->metadata)) {
        if (error) *error = QStringLiteral("PointCloud 仅支持 1-bit、8-bit 和 16-bit 灰度图。");
        return {};
    }
    if (!std::isfinite(settings.pixelScale) || settings.pixelScale <= 0.0 ||
        !std::isfinite(settings.maximumHeight) || settings.maximumHeight <= 0.0) {
        if (error) *error = QStringLiteral("像素比例和最大高度必须大于 0。");
        return {};
    }

    auto document = std::make_shared<PointCloudDocument>();
    document->source = source;
    document->settings = settings;
    document->generation = generation;
    document->rootLevel = rootLevelFor(source->metadata);
    const qint64 step = qint64(1) << document->rootLevel;
    const qint64 sampleColumns = divideRoundUp(source->metadata.width, step);
    const qint64 sampleRows = divideRoundUp(source->metadata.height, step);
    const int tileColumns = static_cast<int>(divideRoundUp(sampleColumns, kPointCloudTileSamples));
    const int tileRows = static_cast<int>(divideRoundUp(sampleRows, kPointCloudTileSamples));
    const qint64 total = qint64(tileColumns) * tileRows;
    document->rootTiles.reserve(static_cast<size_t>(total));

    qint64 completed = 0;
    for (int tileY = 0; tileY < tileRows; ++tileY) {
        for (int tileX = 0; tileX < tileColumns; ++tileX) {
            if (token && token->isCancelled()) return {};
            if (progress) progress({QStringLiteral("生成初始 PointCloud LOD"), completed, total});
            auto tile = buildTile(document, document->rootLevel, tileX, tileY, token, error);
            if (!tile) return {};
            document->rootTiles.push_back(std::move(tile));
            ++completed;
        }
    }
    if (progress) progress({QStringLiteral("生成初始 PointCloud LOD"), total, total});
    return document;
}

PointCloudTilePtr PointCloudBuilder::buildTile(
    const PointCloudDocumentPtr& document, int level, int tileX, int tileY,
    const CancellationTokenPtr& token, QString* error) {
    if (!document || !document->source || !document->source->image || level < 0 || level > 30 ||
        tileX < 0 || tileY < 0 || (token && token->isCancelled())) return {};

    VipsImage* singleBand = nullptr;
    VipsImage* asFloat = nullptr;
    VipsImage* reduced = nullptr;
    VipsImage* cropped = nullptr;
    VipsImage* source = document->source->image.get();
    if (source->Bands == 1) {
        singleBand = source;
        g_object_ref(singleBand);
    } else if (vips_extract_band(source, &singleBand, 0, nullptr) != 0) {
        goto fail;
    }
    if (vips_cast(singleBand, &asFloat, VIPS_FORMAT_FLOAT, nullptr) != 0) goto fail;
    if (level == 0) {
        reduced = asFloat;
        g_object_ref(reduced);
    } else {
        const double factor = static_cast<double>(quint64(1) << level);
        if (vips_shrink(asFloat, &reduced, factor, factor, nullptr) != 0) goto fail;
    }

    {
        const int left = tileX * kPointCloudTileSamples;
        const int top = tileY * kPointCloudTileSamples;
        if (left >= reduced->Xsize || top >= reduced->Ysize) {
            if (error) *error = QStringLiteral("PointCloud LOD 图块超出图像范围。");
            goto fail;
        }
        const int width = std::min(kPointCloudTileSamples, reduced->Xsize - left);
        const int height = std::min(kPointCloudTileSamples, reduced->Ysize - top);
        if (vips_crop(reduced, &cropped, left, top, width, height, nullptr) != 0) goto fail;
    }

    {
        size_t bytes = 0;
        void* memory = vips_image_write_to_memory(cropped, &bytes);
        if (!memory) goto fail;
        auto tile = std::make_shared<PointCloudTile>();
        tile->level = level;
        tile->tileX = tileX;
        tile->tileY = tileY;
        const int width = cropped->Xsize;
        const int height = cropped->Ysize;
        tile->vertices.reserve(static_cast<size_t>(width) * height);
        const auto* values = static_cast<const float*>(memory);
        const qint64 step = qint64(1) << level;
        const float divisor = maximumStoredValue(document->source->metadata.kind);
        for (int y = 0; y < height; ++y) {
            if (token && token->isCancelled()) {
                g_free(memory);
                g_object_unref(cropped); g_object_unref(reduced);
                g_object_unref(asFloat); g_object_unref(singleBand);
                return {};
            }
            for (int x = 0; x < width; ++x) {
                const qint64 sampleX = qint64(tileX * kPointCloudTileSamples + x) * step;
                const qint64 sampleY = qint64(tileY * kPointCloudTileSamples + y) * step;
                const qint64 blockWidth = std::min(step, document->source->metadata.width - sampleX);
                const qint64 blockHeight = std::min(step, document->source->metadata.height - sampleY);
                const float pixelX = static_cast<float>(sampleX + (blockWidth - 1) * 0.5);
                const float pixelY = static_cast<float>(sampleY + (blockHeight - 1) * 0.5);
                const float gray = std::clamp(values[y * width + x] / divisor, 0.0F, 1.0F);
                tile->vertices.push_back({pixelX, pixelY, gray});
            }
        }
        g_free(memory);
        g_object_unref(cropped); g_object_unref(reduced);
        g_object_unref(asFloat); g_object_unref(singleBand);
        return tile;
    }

fail:
    if (cropped) g_object_unref(cropped);
    if (reduced) g_object_unref(reduced);
    if (asFloat) g_object_unref(asFloat);
    if (singleBand) g_object_unref(singleBand);
    if (!(token && token->isCancelled()) && error) *error = VipsSupport::lastError();
    return {};
}

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

bool comesBefore(const PointCloudExtremum& candidate, const PointCloudExtremum& current) {
    return candidate.pixelY < current.pixelY ||
           (candidate.pixelY == current.pixelY && candidate.pixelX < current.pixelX);
}

void considerExtremum(double normalizedGray, qint64 pixelX, qint64 pixelY,
                      std::optional<PointCloudExtremum>* minimum,
                      std::optional<PointCloudExtremum>* maximum) {
    const PointCloudExtremum candidate{pixelX, pixelY,
        std::clamp(normalizedGray, 0.0, 1.0)};
    if (!*minimum || candidate.normalizedGray < (*minimum)->normalizedGray ||
        (candidate.normalizedGray == (*minimum)->normalizedGray &&
         comesBefore(candidate, **minimum))) {
        *minimum = candidate;
    }
    if (!*maximum || candidate.normalizedGray > (*maximum)->normalizedGray ||
        (candidate.normalizedGray == (*maximum)->normalizedGray &&
         comesBefore(candidate, **maximum))) {
        *maximum = candidate;
    }
}

void assignHeightExtrema(const PointCloudDocumentPtr& document,
                         const std::optional<PointCloudExtremum>& minimum,
                         const std::optional<PointCloudExtremum>& maximum) {
    if (!document) return;
    document->highestPoint = document->settings.invertHeight ? minimum : maximum;
    document->lowestPoint = document->settings.invertHeight ? maximum : minimum;
}

bool collectFullResolutionExtrema(
    const PointCloudDocumentPtr& document, const CancellationTokenPtr& token,
    const std::function<void(qint64, qint64)>& progress, QString* error) {
    qint64 total = 0;
    for (const auto& tile : document->rootTiles) {
        if (tile) total += static_cast<qint64>(tile->vertices.size());
    }
    if (total <= 0) {
        if (error) *error = QStringLiteral("PointCloud 区域没有可用像素。");
        return false;
    }

    std::optional<PointCloudExtremum> minimum;
    std::optional<PointCloudExtremum> maximum;
    qint64 completed = 0;
    if (progress) progress(0, total);
    for (const auto& tile : document->rootTiles) {
        if (!tile) continue;
        for (const auto& vertex : tile->vertices) {
            if ((completed & 0x0FFF) == 0) {
                if (token && token->isCancelled()) return false;
                if (progress) progress(completed, total);
            }
            considerExtremum(vertex.normalizedGray,
                             static_cast<qint64>(std::llround(vertex.pixelX)),
                             static_cast<qint64>(std::llround(vertex.pixelY)),
                             &minimum, &maximum);
            ++completed;
        }
    }
    if (token && token->isCancelled()) return false;
    if (progress) progress(total, total);
    assignHeightExtrema(document, minimum, maximum);
    return minimum.has_value() && maximum.has_value();
}

void cancelExtremaEvaluation(VipsImage* image, VipsProgress*, gpointer opaque) {
    const auto* token = static_cast<const CancellationTokenPtr*>(opaque);
    if (*token && (*token)->isCancelled()) vips_image_set_kill(image, TRUE);
}

bool scanSourceExtrema(
    const PointCloudDocumentPtr& document, const CancellationTokenPtr& token,
    const std::function<void(qint64, qint64)>& progress, QString* error) {
    if (!document || !document->source || !document->source->image) return false;
    if (token && token->isCancelled()) return false;

    VipsImage* singleBandRaw = nullptr;
    VipsImage* floatRaw = nullptr;
    VipsImage* source = document->source->image.get();
    if (source->Bands == 1) {
        singleBandRaw = source;
        g_object_ref(singleBandRaw);
    } else if (vips_extract_band(source, &singleBandRaw, 0, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return false;
    }
    const auto singleBand = VipsSupport::adopt(singleBandRaw);
    if (vips_cast(singleBand.get(), &floatRaw, VIPS_FORMAT_FLOAT, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return false;
    }
    const auto asFloat = VipsSupport::adopt(floatRaw);
    VipsRegion* region = vips_region_new(asFloat.get());
    if (!region) {
        if (error) *error = VipsSupport::lastError();
        return false;
    }

    CancellationTokenPtr evaluationToken = token;
    vips_image_set_progress(asFloat.get(), TRUE);
    const gulong cancellationHandler = g_signal_connect(
        asFloat.get(), "eval", G_CALLBACK(cancelExtremaEvaluation), &evaluationToken);
    std::optional<PointCloudExtremum> minimum;
    std::optional<PointCloudExtremum> maximum;
    const int width = asFloat->Xsize;
    const int height = asFloat->Ysize;
    const double divisor = maximumStoredValue(document->source->metadata.kind);
    constexpr int kScanStripRows = 64;
    bool succeeded = true;
    if (progress) progress(0, height);
    for (int top = 0; top < height && succeeded; top += kScanStripRows) {
        if (token && token->isCancelled()) { succeeded = false; break; }
        const int rows = std::min(kScanStripRows, height - top);
        VipsRect rectangle{0, top, width, rows};
        if (vips_region_prepare(region, &rectangle) != 0) {
            succeeded = false;
            break;
        }
        for (int y = top; y < top + rows; ++y) {
            if (token && token->isCancelled()) { succeeded = false; break; }
            const auto* row = reinterpret_cast<const float*>(VIPS_REGION_ADDR(region, 0, y));
            for (int x = 0; x < width; ++x) {
                considerExtremum(row[x] / divisor, x, y, &minimum, &maximum);
            }
        }
        if (progress) progress(std::min(top + rows, height), height);
    }

    if (cancellationHandler) g_signal_handler_disconnect(asFloat.get(), cancellationHandler);
    g_object_unref(region);
    if (!succeeded) {
        if (token && token->isCancelled()) VipsSupport::lastError();
        else if (error) *error = VipsSupport::lastError();
        return false;
    }
    assignHeightExtrema(document, minimum, maximum);
    return minimum.has_value() && maximum.has_value();
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

PointCloudDocumentPtr PointCloudBuilder::buildRegion(
    const ImageDocumentPtr& source, const ImageRegion& region,
    const PointCloudSettings& settings, quint64 generation,
    const CancellationTokenPtr& token, const std::function<void(const TaskProgress&)>& progress,
    QString* error) {
    if (!source || !source->image || !ImageLoader::isProcessable(source->metadata) ||
        !region.isValid() || region.x < 0 || region.y < 0 ||
        region.x + region.width > source->metadata.width ||
        region.y + region.height > source->metadata.height ||
        region.width > std::numeric_limits<int>::max() ||
        region.height > std::numeric_limits<int>::max() ||
        region.x > std::numeric_limits<int>::max() ||
        region.y > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("单要素 PointCloud 区域无效或源图格式不受支持。");
        return {};
    }
    if (token && token->isCancelled()) return {};

    VipsImage* cropped = nullptr;
    if (vips_crop(source->image.get(), &cropped, static_cast<int>(region.x),
                  static_cast<int>(region.y), static_cast<int>(region.width),
                  static_cast<int>(region.height), nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }

    auto croppedDocument = std::make_shared<ImageDocument>(*source);
    croppedDocument->displayName = QStringLiteral("%1 [%2,%3 %4×%5]")
        .arg(source->displayName).arg(region.x).arg(region.y).arg(region.width).arg(region.height);
    croppedDocument->metadata.width = region.width;
    croppedDocument->metadata.height = region.height;
    croppedDocument->image = VipsSupport::adopt(cropped);
    croppedDocument->generation = generation;

    constexpr qint64 kBuildProgressEnd = 600;
    constexpr qint64 kTotalProgress = 1000;
    const auto buildProgress = [progress](const TaskProgress& value) {
        if (!progress) return;
        const double ratio = value.total > 0
            ? std::clamp(static_cast<double>(value.completed) / value.total, 0.0, 1.0)
            : 0.0;
        progress({value.phase, static_cast<qint64>(std::llround(ratio * kBuildProgressEnd)),
                  kTotalProgress});
    };
    auto document = build(croppedDocument, settings, generation, token, buildProgress, error);
    if (!document || (token && token->isCancelled())) return {};

    const QString extremaPhase = QStringLiteral("计算真实 Z 轴最高值和最低值");
    const auto extremaProgress = [progress, &extremaPhase](qint64 completed, qint64 total) {
        if (!progress) return;
        const double ratio = total > 0
            ? std::clamp(static_cast<double>(completed) / total, 0.0, 1.0)
            : 0.0;
        const qint64 mapped = kBuildProgressEnd + static_cast<qint64>(std::llround(
            ratio * (kTotalProgress - kBuildProgressEnd)));
        progress({extremaPhase, mapped, kTotalProgress});
    };
    const bool extremaReady = document->rootLevel == 0
        ? collectFullResolutionExtrema(document, token, extremaProgress, error)
        : scanSourceExtrema(document, token, extremaProgress, error);
    if (!extremaReady || (token && token->isCancelled())) return {};
    if (progress) progress({extremaPhase, kTotalProgress, kTotalProgress});
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

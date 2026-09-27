#include "imaging/ElementGrouping.h"

#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"

#include <QByteArray>
#include <QCache>
#include <QHash>
#include <qhashfunctions.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <vips/vips.h>

namespace {
constexpr int kRepresentativeCacheKiB = 128 * 1024;

struct OwnedElementRaster {
    QByteArray pixels;
    int width = 0;
    int height = 0;
    qsizetype rowBytes = 0;
    PixelKind kind = PixelKind::Unsupported;

    [[nodiscard]] ElementRasterView view() const noexcept {
        return {reinterpret_cast<const uchar*>(pixels.constData()), width, height,
                rowBytes, rowBytes, kind};
    }
};

std::optional<OwnedElementRaster> copyRaster(const ElementRasterView& source) {
    if (!source.isValid() || source.rowBytes > std::numeric_limits<qsizetype>::max() /
                                                   source.height)
        return std::nullopt;
    OwnedElementRaster result;
    result.width = source.width;
    result.height = source.height;
    result.rowBytes = source.rowBytes;
    result.kind = source.kind;
    result.pixels.resize(source.rowBytes * source.height);
    if (result.pixels.size() != source.rowBytes * source.height) return std::nullopt;
    for (int row = 0; row < source.height; ++row)
        std::memcpy(result.pixels.data() + qsizetype(row) * source.rowBytes,
                    source.scanLine(row), static_cast<size_t>(source.rowBytes));
    return result;
}

int cacheCostKiB(const OwnedElementRaster& raster) {
    const qsizetype kib = std::max<qsizetype>(1, (raster.pixels.size() + 1023) / 1024);
    return static_cast<int>(std::min<qsizetype>(kib, std::numeric_limits<int>::max()));
}

VipsImagePtr prepareGroupingImage(const ImageDocumentPtr& document, QString* error) {
    VipsImage* bandRaw = nullptr;
    if (document->image->Bands == 1) {
        bandRaw = document->image.get();
        g_object_ref(bandRaw);
    } else if (vips_extract_band(document->image.get(), &bandRaw, 0, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    const auto band = VipsSupport::adopt(bandRaw);
    const VipsBandFormat target = document->metadata.kind == PixelKind::Gray16
        ? VIPS_FORMAT_USHORT : VIPS_FORMAT_UCHAR;
    VipsImagePtr normalized = band;
    if (band->BandFmt != target) {
        VipsImage* convertedRaw = nullptr;
        if (vips_cast(band.get(), &convertedRaw, target, nullptr) != 0) {
            if (error) *error = VipsSupport::lastError();
            return {};
        }
        normalized = VipsSupport::adopt(convertedRaw);
    }
    VipsImage* copyRaw = nullptr;
    if (vips_copy(normalized.get(), &copyRaw, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    return VipsSupport::adopt(copyRaw);
}

class ElementRasterReader final {
public:
    ElementRasterReader(const VipsImagePtr& image, PixelKind kind)
        : image_(image), kind_(kind), bytesPerSample_(kind == PixelKind::Gray16 ? 2 : 1),
          region_(image ? vips_region_new(image.get()) : nullptr) {}
    ~ElementRasterReader() { if (region_) g_object_unref(region_); }
    ElementRasterReader(const ElementRasterReader&) = delete;
    ElementRasterReader& operator=(const ElementRasterReader&) = delete;

    [[nodiscard]] ElementRasterView read(const ImageRegion& source, QString* error) {
        if (!region_ || !source.isValid() || source.x < 0 || source.y < 0 ||
            source.x > INT_MAX || source.y > INT_MAX || source.width > INT_MAX ||
            source.height > INT_MAX) {
            if (error) *error = QStringLiteral("图元区域无效。");
            return {};
        }
        VipsRect rectangle{static_cast<int>(source.x), static_cast<int>(source.y),
                           static_cast<int>(source.width), static_cast<int>(source.height)};
        if (vips_region_prepare(region_, &rectangle) != 0) {
            if (error) *error = VipsSupport::lastError();
            return {};
        }
        return {VIPS_REGION_ADDR(region_, rectangle.left, rectangle.top), rectangle.width,
                rectangle.height, qsizetype(rectangle.width) * bytesPerSample_,
                static_cast<qsizetype>(VIPS_REGION_LSKIP(region_)), kind_};
    }

private:
    VipsImagePtr image_;
    PixelKind kind_ = PixelKind::Unsupported;
    qsizetype bytesPerSample_ = 1;
    VipsRegion* region_ = nullptr;
};

class EvaluationCancellation final {
public:
    EvaluationCancellation(VipsImage* image, const CancellationTokenPtr& token)
        : image_(image), token_(token) {
        if (!image_ || !token_) return;
        vips_image_set_progress(image_, TRUE);
        handler_ = g_signal_connect(image_, "eval", G_CALLBACK(cancelEvaluation), this);
    }
    ~EvaluationCancellation() {
        if (handler_) g_signal_handler_disconnect(image_, handler_);
    }
    EvaluationCancellation(const EvaluationCancellation&) = delete;
    EvaluationCancellation& operator=(const EvaluationCancellation&) = delete;

private:
    static void cancelEvaluation(VipsImage* image, VipsProgress*, gpointer opaque) {
        const auto* scope = static_cast<const EvaluationCancellation*>(opaque);
        if (scope->token_ && scope->token_->isCancelled()) vips_image_set_kill(image, TRUE);
    }

    VipsImage* image_ = nullptr;
    CancellationTokenPtr token_;
    gulong handler_ = 0;
};

bool sameSettings(const SegmentationSettings& left, const SegmentationSettings& right) {
    return left.cellWidth == right.cellWidth && left.cellHeight == right.cellHeight &&
           left.threshold == right.threshold && left.polarity == right.polarity;
}

bool isPureBackground(const ElementRasterView& raster) {
    if (!raster.isValid() ||
        (raster.kind != PixelKind::Gray8 && raster.kind != PixelKind::Gray16))
        return false;
    for (int row = 0; row < raster.height; ++row) {
        const uchar* pixels = raster.scanLine(row);
        for (qsizetype byte = 0; byte < raster.rowBytes; ++byte)
            if (pixels[byte] != 0xffU) return false;
    }
    return true;
}
}

int ElementGroupingResult::groupIdAt(int row, int column) const noexcept {
    if (!valid || row < 0 || column < 0 || row >= rows || column >= columns) return -1;
    const qsizetype index = qsizetype(row) * columns + column;
    return index >= 0 && index < groupByCell.size() ? groupByCell[index] : -1;
}

bool ElementGroupingResult::isBackgroundAt(int row, int column) const noexcept {
    return valid && row >= 0 && column >= 0 && row < rows && column < columns &&
           groupIdAt(row, column) == kBackgroundElementGroupId;
}

qint64 ElementGroupingResult::backgroundCellCount() const noexcept {
    if (!valid) return 0;
    return std::count(groupByCell.cbegin(), groupByCell.cend(),
                      kBackgroundElementGroupId);
}

QColor ElementGroupingResult::colorAt(int row, int column) const {
    const int groupId = groupIdAt(row, column);
    return groupId >= 0 && groupId < groups.size() ? groups[groupId].color : QColor{};
}

bool ElementGroupingResult::matches(const ImageDocumentPtr& document,
                                    const SegmentationResult& segmentation) const noexcept {
    return valid && document && document->generation == documentGeneration &&
           rows == segmentation.rows && columns == segmentation.columns &&
           sameSettings(settings, segmentation.settings) &&
           groupByCell.size() == qsizetype(rows) * columns;
}

QString ExactGrayElementComparison::id() const { return QStringLiteral("exact_gray_v1"); }

ElementFingerprint ExactGrayElementComparison::fingerprint(
    const ElementRasterView& raster) const {
    if (!raster.isValid()) return 0;
    const std::array<quint64,4> header{
        static_cast<quint64>(raster.width), static_cast<quint64>(raster.height),
        static_cast<quint64>(raster.rowBytes), static_cast<quint64>(raster.kind)};
    size_t hash = qHashBits(header.data(), sizeof(header));
    for (int row = 0; row < raster.height; ++row)
        hash = qHashBits(raster.scanLine(row), static_cast<size_t>(raster.rowBytes), hash);
    return static_cast<ElementFingerprint>(hash);
}

bool ExactGrayElementComparison::equivalent(const ElementRasterView& left,
                                            const ElementRasterView& right) const {
    if (!left.isValid() || !right.isValid() || left.width != right.width ||
        left.height != right.height || left.rowBytes != right.rowBytes ||
        left.kind != right.kind)
        return false;
    for (int row = 0; row < left.height; ++row) {
        if (std::memcmp(left.scanLine(row), right.scanLine(row),
                        static_cast<size_t>(left.rowBytes)) != 0)
            return false;
    }
    return true;
}

QColor elementGroupColor(int groupId) {
    if (groupId < 0) return {};
    const int hue = (groupId * 137 + 17) % 360;
    const int saturation = 150 + ((groupId / 360) * 29) % 86;
    const int value = 190 + ((groupId / (360 * 3)) * 23) % 66;
    return QColor::fromHsv(hue, saturation, value);
}

ElementGroupingResult ElementGrouper::group(
    const ImageDocumentPtr& document, const SegmentationResult& segmentation,
    const ElementComparisonStrategy& strategy, const CancellationTokenPtr& token,
    const Progress& progress, QString* error) {
    if (error) error->clear();
    ElementGroupingResult result;
    if (!document || !document->image || !ImageLoader::isProcessable(document->metadata) ||
        segmentation.rows <= 0 || segmentation.columns <= 0 ||
        segmentation.settings.cellWidth <= 0 || segmentation.settings.cellHeight <= 0 ||
        !token) {
        if (error) *error = QStringLiteral("图元分组参数无效。");
        return result;
    }

    const qint64 total = qint64(segmentation.rows) * segmentation.columns;
    if (total <= 0 || total > kMaximumSegmentationCells) {
        if (error) *error = QStringLiteral("图元数量无效或超过程序限制。");
        return result;
    }

    result.documentGeneration = document->generation;
    result.settings = segmentation.settings;
    result.rows = segmentation.rows;
    result.columns = segmentation.columns;
    result.strategyId = strategy.id();
    result.groupByCell.fill(-1, static_cast<qsizetype>(total));

    QString prepareError;
    const VipsImagePtr prepared = prepareGroupingImage(document, &prepareError);
    if (!prepared) {
        if (error) *error = QStringLiteral("准备图元分组数据失败：%1").arg(prepareError);
        return {};
    }
    EvaluationCancellation cancellation(prepared.get(), token);
    ElementRasterReader currentReader(prepared, document->metadata.kind);
    ElementRasterReader representativeReader(prepared, document->metadata.kind);
    QHash<ElementFingerprint, QVector<int>> candidates;
    QCache<int, OwnedElementRaster> representativeCache(kRepresentativeCacheKiB);

    qint64 completed = 0;
    for (int row = 0; row < segmentation.rows; ++row) {
        for (int column = 0; column < segmentation.columns; ++column) {
            if (token->isCancelled()) return {};
            const ImageRegion region = segmentationCellRegion(
                segmentation, document->metadata.width, document->metadata.height,
                row, column);
            if (!region.isValid()) {
                if (error) *error = QStringLiteral("分割网格与当前图像尺寸不匹配。");
                return {};
            }
            QString loadError;
            const ElementRasterView raster = currentReader.read(region, &loadError);
            if (!raster.isValid()) {
                if (!token->isCancelled() && error)
                    *error = QStringLiteral("读取图元（行 %1，列 %2）失败：%3")
                                 .arg(row + 1).arg(column + 1).arg(loadError);
                return {};
            }

            const QPoint cell(column, row);
            if (isPureBackground(raster)) {
                result.groupByCell[qsizetype(row) * segmentation.columns + column] =
                    kBackgroundElementGroupId;
                ++completed;
                if (progress && (completed == total || completed % 32 == 0))
                    progress({QStringLiteral("比较并分组图元"), completed, total});
                continue;
            }

            const ElementFingerprint fingerprint = strategy.fingerprint(raster);
            int groupId = -1;
            const auto bucket = candidates.constFind(fingerprint);
            if (bucket != candidates.cend()) {
                for (int candidateId : bucket.value()) {
                    const OwnedElementRaster* representative =
                        representativeCache.object(candidateId);
                    std::optional<OwnedElementRaster> loadedRepresentative;
                    if (!representative) {
                        const QPoint representativeCell =
                            result.groups[candidateId].representativeCell;
                        const ImageRegion representativeRegion = segmentationCellRegion(
                            segmentation, document->metadata.width, document->metadata.height,
                            representativeCell.y(), representativeCell.x());
                        const ElementRasterView representativeView =
                            representativeReader.read(representativeRegion, &loadError);
                        loadedRepresentative = copyRaster(representativeView);
                        if (!loadedRepresentative) {
                            if (!token->isCancelled() && error)
                                *error = QStringLiteral("重新读取代表图元失败：%1")
                                             .arg(loadError);
                            return {};
                        }
                        representative = &*loadedRepresentative;
                    }
                    const bool equivalent = strategy.equivalent(raster, representative->view());
                    if (loadedRepresentative) {
                        const int cost = cacheCostKiB(*loadedRepresentative);
                        if (cost <= kRepresentativeCacheKiB)
                            representativeCache.insert(candidateId,
                                new OwnedElementRaster(std::move(*loadedRepresentative)), cost);
                    }
                    if (equivalent) {
                        groupId = candidateId;
                        break;
                    }
                }
            }
            if (groupId < 0) {
                groupId = result.groups.size();
                result.groups.push_back({groupId, cell, {}, elementGroupColor(groupId)});
                candidates[fingerprint].push_back(groupId);
                auto owned = copyRaster(raster);
                if (!owned) {
                    if (error) *error = QStringLiteral("图元尺寸过大，无法缓存代表图元。");
                    return {};
                }
                const int cost = cacheCostKiB(*owned);
                if (cost <= kRepresentativeCacheKiB)
                    representativeCache.insert(groupId,
                        new OwnedElementRaster(std::move(*owned)), cost);
            }
            result.groups[groupId].cells.push_back(cell);
            result.groupByCell[qsizetype(row) * segmentation.columns + column] = groupId;
            ++completed;
            if (progress && (completed == total || completed % 32 == 0))
                progress({QStringLiteral("比较并分组图元"), completed, total});
        }
    }
    result.valid = true;
    return result;
}

bool ElementGrouper::findPureBackgroundCells(
    const ImageDocumentPtr& document, const SegmentationResult& segmentation,
    QVector<bool>* backgroundByCell, const CancellationTokenPtr& token,
    const Progress& progress, QString* error) {
    if (error) error->clear();
    if (backgroundByCell) backgroundByCell->clear();
    if (!backgroundByCell || !document || !document->image ||
        !ImageLoader::isProcessable(document->metadata) || segmentation.rows <= 0 ||
        segmentation.columns <= 0 || segmentation.settings.cellWidth <= 0 ||
        segmentation.settings.cellHeight <= 0) {
        if (error) *error = QStringLiteral("纯底色分块检测参数无效。");
        return false;
    }
    const qint64 total = qint64(segmentation.rows) * segmentation.columns;
    if (total <= 0 || total > kMaximumSegmentationCells) {
        if (error) *error = QStringLiteral("图元数量无效或超过程序限制。");
        return false;
    }
    backgroundByCell->fill(false, static_cast<qsizetype>(total));
    if (document->metadata.kind != PixelKind::Gray8 &&
        document->metadata.kind != PixelKind::Gray16)
        return true;

    QString prepareError;
    const VipsImagePtr prepared = prepareGroupingImage(document, &prepareError);
    if (!prepared) {
        if (error) *error = QStringLiteral("准备纯底色检测数据失败：%1").arg(prepareError);
        backgroundByCell->clear();
        return false;
    }
    EvaluationCancellation cancellation(prepared.get(), token);
    ElementRasterReader reader(prepared, document->metadata.kind);
    qint64 completed = 0;
    for (int row = 0; row < segmentation.rows; ++row) {
        for (int column = 0; column < segmentation.columns; ++column) {
            if (token && token->isCancelled()) {
                backgroundByCell->clear();
                return false;
            }
            const ImageRegion region = segmentationCellRegion(
                segmentation, document->metadata.width, document->metadata.height,
                row, column);
            QString loadError;
            const ElementRasterView raster = reader.read(region, &loadError);
            if (!raster.isValid()) {
                if (error) *error = QStringLiteral("读取图元（行 %1，列 %2）失败：%3")
                    .arg(row + 1).arg(column + 1).arg(loadError);
                backgroundByCell->clear();
                return false;
            }
            (*backgroundByCell)[qsizetype(row) * segmentation.columns + column] =
                isPureBackground(raster);
            ++completed;
            if (progress && (completed == total || completed % 32 == 0))
                progress({QStringLiteral("检测纯底色分块"), completed, total});
        }
    }
    return true;
}

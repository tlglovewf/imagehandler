#include "imaging/ElementGrouping.h"

#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"

#include <QCache>
#include <QCryptographicHash>
#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <algorithm>
#include <cstring>

namespace {
qsizetype pixelRowBytes(const QImage& image) {
    if (image.format() == QImage::Format_Grayscale16)
        return qsizetype(image.width()) * qsizetype(sizeof(quint16));
    return image.width();
}

bool sameSettings(const SegmentationSettings& left, const SegmentationSettings& right) {
    return left.cellWidth == right.cellWidth && left.cellHeight == right.cellHeight &&
           left.threshold == right.threshold && left.polarity == right.polarity;
}
}

int ElementGroupingResult::groupIdAt(int row, int column) const noexcept {
    if (!valid || row < 0 || column < 0 || row >= rows || column >= columns) return -1;
    const qsizetype index = qsizetype(row) * columns + column;
    return index >= 0 && index < groupByCell.size() ? groupByCell[index] : -1;
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

QByteArray ExactGrayElementComparison::fingerprint(const QImage& image, PixelKind kind) const {
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << qint32(image.width()) << qint32(image.height()) << qint32(image.format())
           << qint32(kind);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(header);
    const qsizetype rowBytes = pixelRowBytes(image);
    for (int y = 0; y < image.height(); ++y)
        hash.addData(reinterpret_cast<const char*>(image.constScanLine(y)), rowBytes);
    return hash.result();
}

bool ExactGrayElementComparison::equivalent(const QImage& left, const QImage& right,
                                            PixelKind kind) const {
    Q_UNUSED(kind);
    if (left.size() != right.size() || left.format() != right.format()) return false;
    const qsizetype rowBytes = pixelRowBytes(left);
    for (int y = 0; y < left.height(); ++y) {
        if (std::memcmp(left.constScanLine(y), right.constScanLine(y),
                        static_cast<size_t>(rowBytes)) != 0)
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

    QHash<QByteArray, QVector<int>> candidates;
    QCache<int, QImage> representativeCache(128 * 1024); // KiB
    auto representativeImage = [&](int groupId, QString* loadError) -> QImage {
        if (const auto* cached = representativeCache.object(groupId)) return *cached;
        const QPoint cell = result.groups[groupId].representativeCell;
        const ImageRegion region = segmentationCellRegion(
            segmentation, document->metadata.width, document->metadata.height,
            cell.y(), cell.x());
        QImage image = VipsSupport::renderRegion(document, region.x, region.y,
                                                 region.width, region.height, 1.0,
                                                 loadError, token);
        if (!image.isNull()) {
            representativeCache.insert(groupId, new QImage(image),
                std::max(1, static_cast<int>(image.sizeInBytes() / 1024)));
        }
        return image;
    };

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
            const QImage image = VipsSupport::renderRegion(
                document, region.x, region.y, region.width, region.height, 1.0,
                &loadError, token);
            if (image.isNull()) {
                if (!token->isCancelled() && error)
                    *error = QStringLiteral("读取图元（行 %1，列 %2）失败：%3")
                                 .arg(row + 1).arg(column + 1).arg(loadError);
                return {};
            }

            const QByteArray fingerprint = strategy.fingerprint(image, document->metadata.kind);
            int groupId = -1;
            const auto bucket = candidates.constFind(fingerprint);
            if (bucket != candidates.cend()) {
                for (int candidateId : bucket.value()) {
                    QImage representative = representativeImage(candidateId, &loadError);
                    if (representative.isNull()) {
                        if (!token->isCancelled() && error)
                            *error = QStringLiteral("重新读取代表图元失败：%1").arg(loadError);
                        return {};
                    }
                    if (strategy.equivalent(image, representative, document->metadata.kind)) {
                        groupId = candidateId;
                        break;
                    }
                }
            }
            const QPoint cell(column, row);
            if (groupId < 0) {
                groupId = result.groups.size();
                result.groups.push_back({groupId, cell, {}, elementGroupColor(groupId)});
                candidates[fingerprint].push_back(groupId);
                representativeCache.insert(groupId, new QImage(image),
                    std::max(1, static_cast<int>(image.sizeInBytes() / 1024)));
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

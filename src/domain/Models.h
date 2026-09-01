#pragma once

#include <QImage>
#include <QMetaType>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct _VipsImage;
using VipsImage = _VipsImage;

enum class PixelKind { Binary1, Gray8, Gray16, Color, Unsupported };
enum class ForegroundPolarity { Dark, Light };
enum class TraversalMode { SnakeZ, ClockwiseSpiral };

inline constexpr qint64 kMaximumSegmentationCells = 1'000'000;

struct ImageRegion {
    qint64 x = 0;
    qint64 y = 0;
    qint64 width = 0;
    qint64 height = 0;

    [[nodiscard]] bool isValid() const noexcept { return width > 0 && height > 0; }
};

struct VipsImageDeleter {
    void operator()(VipsImage* image) const noexcept;
};
using VipsImagePtr = std::shared_ptr<VipsImage>;

struct ImageMetadata {
    qint64 width = 0;
    qint64 height = 0;
    int bands = 0;
    int sourceBitsPerPixel = 0;
    PixelKind kind = PixelKind::Unsupported;
};

struct ImageDocument {
    QString displayName;
    QString sourcePath;
    ImageMetadata metadata;
    VipsImagePtr image;
    quint64 generation = 0;
    bool virtualMosaic = false;
};
using ImageDocumentPtr = std::shared_ptr<ImageDocument>;

struct SegmentationSettings {
    int cellWidth = 0;
    int cellHeight = 0;
    int threshold = 128;
    ForegroundPolarity polarity = ForegroundPolarity::Dark;
};

struct SegmentationResult {
    SegmentationSettings settings;
    int rows = 0;
    int columns = 0;
    int remainderWidth = 0;
    int remainderHeight = 0;
    double confidence = 0.0;
    QString warning;
};

inline ImageRegion segmentationCellRegion(const SegmentationResult& segmentation,
                                          qint64 imageWidth, qint64 imageHeight,
                                          int row, int column) {
    if (row < 0 || column < 0 || row >= segmentation.rows ||
        column >= segmentation.columns || segmentation.settings.cellWidth <= 0 ||
        segmentation.settings.cellHeight <= 0 || imageWidth <= 0 || imageHeight <= 0) {
        return {};
    }
    const qint64 x = qint64(column) * segmentation.settings.cellWidth;
    const qint64 y = qint64(row) * segmentation.settings.cellHeight;
    if (x >= imageWidth || y >= imageHeight) return {};
    return {x, y,
            std::min<qint64>(segmentation.settings.cellWidth, imageWidth - x),
            std::min<qint64>(segmentation.settings.cellHeight, imageHeight - y)};
}

struct MergeTile {
    QString path;
    QString prefix;
    int row = 0;
    int column = 0;
};

struct TaskProgress {
    QString phase;
    qint64 completed = 0;
    qint64 total = 0;
};

class CancellationToken {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool isCancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }
private:
    std::atomic_bool cancelled_{false};
};
using CancellationTokenPtr = std::shared_ptr<CancellationToken>;

Q_DECLARE_METATYPE(ImageDocumentPtr)
Q_DECLARE_METATYPE(SegmentationResult)

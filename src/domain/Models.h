#pragma once

#include <QImage>
#include <QMetaType>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct _VipsImage;
using VipsImage = _VipsImage;

enum class PixelKind { Binary1, Gray8, Color, Unsupported };
enum class ForegroundPolarity { Dark, Light };
enum class TraversalMode { SnakeZ, ClockwiseSpiral };

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


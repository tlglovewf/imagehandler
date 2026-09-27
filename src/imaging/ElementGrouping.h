#pragma once

#include "domain/Models.h"

#include <QColor>
#include <QMetaType>
#include <QPoint>
#include <QVector>
#include <QtTypes>
#include <functional>

inline constexpr int kBackgroundElementGroupId = -1;

struct ElementGroup {
    int id = -1;
    QPoint representativeCell{-1, -1};
    QVector<QPoint> cells;
    QColor color;
};

struct ElementGroupingResult {
    quint64 documentGeneration = 0;
    SegmentationSettings settings;
    int rows = 0;
    int columns = 0;
    QVector<int> groupByCell;
    QVector<ElementGroup> groups;
    QString strategyId;
    bool valid = false;

    [[nodiscard]] int groupIdAt(int row, int column) const noexcept;
    [[nodiscard]] bool isBackgroundAt(int row, int column) const noexcept;
    [[nodiscard]] qint64 backgroundCellCount() const noexcept;
    [[nodiscard]] QColor colorAt(int row, int column) const;
    [[nodiscard]] bool matches(const ImageDocumentPtr& document,
                               const SegmentationResult& segmentation) const noexcept;
};

struct ElementRasterView {
    const uchar* data = nullptr;
    int width = 0;
    int height = 0;
    qsizetype rowBytes = 0;
    qsizetype stride = 0;
    PixelKind kind = PixelKind::Unsupported;

    [[nodiscard]] bool isValid() const noexcept {
        return data && width > 0 && height > 0 && rowBytes > 0 && stride >= rowBytes;
    }
    [[nodiscard]] const uchar* scanLine(int row) const noexcept {
        return isValid() && row >= 0 && row < height ? data + qsizetype(row) * stride : nullptr;
    }
};

using ElementFingerprint = quint64;

// A strategy supplies a cheap candidate key and a definitive comparison. Future
// tolerant or transform-aware matchers can be added without changing grouping,
// preview, Job, or export code.
class ElementComparisonStrategy {
public:
    virtual ~ElementComparisonStrategy() = default;
    [[nodiscard]] virtual QString id() const = 0;
    [[nodiscard]] virtual ElementFingerprint fingerprint(
        const ElementRasterView& raster) const = 0;
    [[nodiscard]] virtual bool equivalent(const ElementRasterView& left,
                                          const ElementRasterView& right) const = 0;
};

class ExactGrayElementComparison final : public ElementComparisonStrategy {
public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] ElementFingerprint fingerprint(
        const ElementRasterView& raster) const override;
    [[nodiscard]] bool equivalent(const ElementRasterView& left,
                                  const ElementRasterView& right) const override;
};

class ElementGrouper final {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    [[nodiscard]] static ElementGroupingResult group(
        const ImageDocumentPtr& document, const SegmentationResult& segmentation,
        const ElementComparisonStrategy& strategy, const CancellationTokenPtr& token,
        const Progress& progress, QString* error);
    [[nodiscard]] static bool findPureBackgroundCells(
        const ImageDocumentPtr& document, const SegmentationResult& segmentation,
        QVector<bool>* backgroundByCell, const CancellationTokenPtr& token = {},
        const Progress& progress = {}, QString* error = nullptr);
};

[[nodiscard]] QColor elementGroupColor(int groupId);

Q_DECLARE_METATYPE(ElementGroupingResult)

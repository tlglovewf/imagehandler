#pragma once

#include "domain/Models.h"

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QMetaType>
#include <QPoint>
#include <QVector>
#include <functional>

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
    [[nodiscard]] QColor colorAt(int row, int column) const;
    [[nodiscard]] bool matches(const ImageDocumentPtr& document,
                               const SegmentationResult& segmentation) const noexcept;
};

// A strategy supplies a cheap candidate key and a definitive comparison. Future
// tolerant or transform-aware matchers can be added without changing grouping,
// preview, Job, or export code.
class ElementComparisonStrategy {
public:
    virtual ~ElementComparisonStrategy() = default;
    [[nodiscard]] virtual QString id() const = 0;
    [[nodiscard]] virtual QByteArray fingerprint(const QImage& image,
                                                 PixelKind kind) const = 0;
    [[nodiscard]] virtual bool equivalent(const QImage& left, const QImage& right,
                                          PixelKind kind) const = 0;
};

class ExactGrayElementComparison final : public ElementComparisonStrategy {
public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] QByteArray fingerprint(const QImage& image,
                                         PixelKind kind) const override;
    [[nodiscard]] bool equivalent(const QImage& left, const QImage& right,
                                  PixelKind kind) const override;
};

class ElementGrouper final {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    [[nodiscard]] static ElementGroupingResult group(
        const ImageDocumentPtr& document, const SegmentationResult& segmentation,
        const ElementComparisonStrategy& strategy, const CancellationTokenPtr& token,
        const Progress& progress, QString* error);
};

[[nodiscard]] QColor elementGroupColor(int groupId);

Q_DECLARE_METATYPE(ElementGroupingResult)

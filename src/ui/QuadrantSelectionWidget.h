#pragma once

#include "domain/Models.h"

#include <QImage>
#include <QRect>
#include <QSet>
#include <QWidget>

class QLabel;
class QPushButton;

enum class ImageQuadrant { TopLeft, TopRight, BottomLeft, BottomRight };

class QuadrantSelectionWidget final : public QWidget {
    Q_OBJECT
public:
    explicit QuadrantSelectionWidget(QWidget* parent = nullptr);
    ~QuadrantSelectionWidget() override;

    void setSource(const ImageDocumentPtr& document,
                   const SegmentationResult& segmentation);
    void setSelectedCells(const QVector<QPoint>& cells);
    void clear();

    [[nodiscard]] static QRect cellBoundsForQuadrant(
        const SegmentationResult& segmentation, ImageQuadrant quadrant);
    [[nodiscard]] static QVector<QPoint> cellsForQuadrant(
        const SegmentationResult& segmentation, ImageQuadrant quadrant);
    [[nodiscard]] static QVector<QPoint> allCells(
        const SegmentationResult& segmentation);
    [[nodiscard]] quint64 displayedThumbnailGeneration() const noexcept {
        return displayedThumbnailGeneration_;
    }
    [[nodiscard]] bool isQuadrantSelected(ImageQuadrant quadrant) const;

Q_SIGNALS:
    void quadrantSelected(ImageQuadrant quadrant, bool additive);
    void selectionPreviewChanged(const QRect& cellBounds);
    void selectAllRequested();
    void clearRequested();
    void thumbnailReady(quint64 generation);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    QSize sizeHint() const override;

private:
    [[nodiscard]] bool hasGrid() const noexcept;
    [[nodiscard]] QRect canvasRect() const;
    [[nodiscard]] QRectF imageRect() const;
    [[nodiscard]] QPointF splitPoint(const QRectF& image) const;
    [[nodiscard]] ImageQuadrant quadrantAt(const QPointF& position) const;
    [[nodiscard]] QRectF quadrantRect(const QRectF& image,
                                      ImageQuadrant quadrant) const;
    void updateHoveredQuadrant(const QPointF& position);
    void startThumbnailLoad();

    ImageDocumentPtr document_;
    SegmentationResult segmentation_;
    QSet<QPoint> selectedCells_;
    QImage thumbnail_;
    CancellationTokenPtr thumbnailToken_;
    quint64 thumbnailSerial_ = 0;
    quint64 displayedThumbnailGeneration_ = 0;
    int hoveredQuadrant_ = -1;
    QPushButton* selectAllButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QLabel* hintLabel_ = nullptr;
};

Q_DECLARE_METATYPE(ImageQuadrant)

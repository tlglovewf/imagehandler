#pragma once

#include "domain/Models.h"
#include "imaging/ElementGrouping.h"
#include <QCache>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QWidget>

class QKeyEvent;
class QPainter;
class QFocusEvent;

enum class FixedGrayPointState { Loading, Ready, Failed };

struct FixedGrayPoint {
    quint64 id = 0;
    qint64 x = 0;
    qint64 y = 0;
    quint32 value = 0;
    FixedGrayPointState state = FixedGrayPointState::Loading;
};

class LargeImageView final : public QWidget {
    Q_OBJECT
public:
    explicit LargeImageView(QWidget* parent = nullptr);
    void setDocument(const ImageDocumentPtr& document);
    void setSegmentation(const SegmentationResult& segmentation);
    void clearSegmentation();
    void setElementGrouping(const ElementGroupingResult& grouping);
    void clearElementGrouping();
    void setTraversalMode(TraversalMode mode);
    void setTraversalPathVisible(bool visible);
    void setSelectedCell(int row, int column, bool centerOnSelection = false);
    void centerOnCell(int row, int column);
    [[nodiscard]] QPoint selectedCell() const { return selectedCell_; }
    [[nodiscard]] QVector<QPoint> selectedCells() const;
    [[nodiscard]] const QVector<FixedGrayPoint>& fixedGrayPoints() const noexcept {
        return fixedGrayPoints_;
    }
    double zoom() const { return zoom_; }

public Q_SLOTS:
    void fitToWindow();
    void actualSize();
    void clearFixedGrayPoints();

Q_SIGNALS:
    void viewStatusChanged(double zoom, qint64 width, qint64 height);
    void tileProgress(quint64 generation, qint64 completed, qint64 failed, qint64 total);
    void tileError(const QString& error);
    void segmentationCellSelected(int row, int column);
    void segmentationCellsSelected(const QVector<QPoint>& cells, const QPoint& activeCell);
    void segmentationCellActivated(int row, int column);
    void segmentationCellSliceActivated(int row, int column);
    void segmentationCellsSliceActivated(const QVector<QPoint>& cells,
                                         const QPoint& activeCell);
    void pixelValueChanged(qint64 x, qint64 y, quint32 value, bool valid);
    void fixedGrayPointCountChanged(int count);
    void fixedGrayPointError(const QString& error);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    bool event(QEvent*) override;
    void leaveEvent(QEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    QString tileKey(int level, int tx, int ty) const;
    void requestTile(const QString& key, int level, int tx, int ty, int sourceTile);
    void emitTileProgress();
    [[nodiscard]] QPoint cellAt(const QPoint& viewportPosition) const;
    void selectCellAt(const QPoint& viewportPosition);
    void toggleCellAt(const QPoint& viewportPosition);
    void selectCellsInRect(const QRect& viewportRect, const QPoint& releasePosition);
    void setSingleSelectedCell(const QPoint& cell, bool notify = true);
    void setActiveCell(const QPoint& cell, bool notify = true);
    void emitSelectionChanged();
    void updatePixelProbe(const QPoint& viewportPosition);
    void requestProbeBlock(qint64 x, qint64 y);
    void emitCachedProbe();
    void addFixedGrayPointAt(const QPoint& viewportPosition);
    [[nodiscard]] quint64 fixedGrayPointAt(const QPoint& viewportPosition) const;
    [[nodiscard]] QPointF fixedGrayPointPosition(const FixedGrayPoint& point) const;
    [[nodiscard]] QRectF fixedGrayPointLabelRect(const FixedGrayPoint& point) const;
    [[nodiscard]] QString fixedGrayPointLabel(const FixedGrayPoint& point) const;
    void drawFixedGrayPoints(QPainter& painter) const;
    void removeSelectedFixedGrayPoint();
    void clearSelectedCell();
    void centerImage();
    void emitStatus();

    ImageDocumentPtr document_;
    SegmentationResult segmentation_;
    ElementGroupingResult grouping_;
    bool showGrid_ = false;
    bool showTraversalPath_ = false;
    TraversalMode traversalMode_ = TraversalMode::SnakeZ;
    QPoint selectedCell_{-1, -1};
    QSet<QPoint> selectedCells_;
    double zoom_ = 1.0;
    QPointF offset_;
    QPoint pressPosition_;
    QPoint lastMouse_;
    bool leftPressed_ = false;
    bool dragging_ = false;
    Qt::KeyboardModifiers pressModifiers_;
    bool multiSelectHeld_ = false;
    bool pressMultiSelect_ = false;
    bool boxSelectHeld_ = false;
    bool pressBoxSelect_ = false;
    bool boxSelecting_ = false;
    QPoint boxSelectionStart_;
    QRect boxSelectionRect_;
    QCache<QString, QImage> cache_{256 * 1024};
    QSet<QString> pending_;
    QHash<QString, QString> failedTiles_;
    QSet<QString> visibleTileKeys_;
    CancellationTokenPtr tileRequestToken_;
    QImage probeCache_;
    QPoint probeCacheOrigin_{-1, -1};
    QPoint hoverPixel_{-1, -1};
    bool probePending_ = false;
    quint64 probeRequestSerial_ = 0;
    QVector<FixedGrayPoint> fixedGrayPoints_;
    quint64 selectedFixedGrayPointId_ = 0;
    quint64 nextFixedGrayPointId_ = 1;
};

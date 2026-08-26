#pragma once

#include "domain/Models.h"
#include <QCache>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QWidget>

class LargeImageView final : public QWidget {
    Q_OBJECT
public:
    explicit LargeImageView(QWidget* parent = nullptr);
    void setDocument(const ImageDocumentPtr& document);
    void setSegmentation(const SegmentationResult& segmentation);
    void clearSegmentation();
    void setTraversalMode(TraversalMode mode);
    void setTraversalPathVisible(bool visible);
    double zoom() const { return zoom_; }

public Q_SLOTS:
    void fitToWindow();
    void actualSize();

Q_SIGNALS:
    void viewStatusChanged(double zoom, qint64 width, qint64 height);
    void tileProgress(const QString& phase, qint64 completed, qint64 total);
    void tileError(const QString& error);
    void segmentationCellSelected(int row, int column);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    QString tileKey(int level, int tx, int ty) const;
    void requestTile(const QString& key, int level, int tx, int ty, int sourceTile);
    void selectCellAt(const QPoint& viewportPosition);
    void clearSelectedCell();
    void centerImage();
    void emitStatus();

    ImageDocumentPtr document_;
    SegmentationResult segmentation_;
    bool showGrid_ = false;
    bool showTraversalPath_ = false;
    TraversalMode traversalMode_ = TraversalMode::SnakeZ;
    QPoint selectedCell_{-1, -1};
    double zoom_ = 1.0;
    QPointF offset_;
    QPoint pressPosition_;
    QPoint lastMouse_;
    bool leftPressed_ = false;
    bool dragging_ = false;
    QCache<QString, QImage> cache_{256 * 1024};
    QSet<QString> pending_;
    QHash<QString, QString> failedTiles_;
};

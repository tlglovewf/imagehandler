#pragma once

#include "domain/Models.h"
#include <QCache>
#include <QElapsedTimer>
#include <QSet>
#include <QWidget>

class LargeImageView final : public QWidget {
    Q_OBJECT
public:
    explicit LargeImageView(QWidget* parent = nullptr);
    void setDocument(const ImageDocumentPtr& document);
    void setSegmentation(const SegmentationResult& segmentation);
    void clearSegmentation();
    double zoom() const { return zoom_; }

public Q_SLOTS:
    void fitToWindow();
    void actualSize();

Q_SIGNALS:
    void viewStatusChanged(double zoom, qint64 width, qint64 height);
    void tileProgress(const QString& phase, qint64 completed, qint64 total);

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
    void centerImage();
    void emitStatus();

    ImageDocumentPtr document_;
    SegmentationResult segmentation_;
    bool showGrid_ = false;
    double zoom_ = 1.0;
    QPointF offset_;
    QPoint lastMouse_;
    bool dragging_ = false;
    QCache<QString, QImage> cache_{256 * 1024};
    QSet<QString> pending_;
};

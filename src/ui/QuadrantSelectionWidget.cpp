#include "ui/QuadrantSelectionWidget.h"

#include "imaging/VipsSupport.h"

#include <QFutureWatcher>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr int kBottomControlsHeight = 58;
constexpr int kThumbnailMaximumSide = 384;

QString quadrantName(ImageQuadrant quadrant) {
    switch (quadrant) {
    case ImageQuadrant::TopLeft: return QStringLiteral("左上");
    case ImageQuadrant::TopRight: return QStringLiteral("右上");
    case ImageQuadrant::BottomLeft: return QStringLiteral("左下");
    case ImageQuadrant::BottomRight: return QStringLiteral("右下");
    }
    return {};
}

struct ThumbnailResult {
    quint64 serial = 0;
    quint64 generation = 0;
    QImage image;
};
}

QuadrantSelectionWidget::QuadrantSelectionWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("quadrantSelectionWidget"));
    setMouseTracking(true);
    setMinimumHeight(185);

    selectAllButton_ = new QPushButton(QStringLiteral("全选"), this);
    selectAllButton_->setObjectName(QStringLiteral("quadrantSelectAllButton"));
    clearButton_ = new QPushButton(QStringLiteral("清空"), this);
    clearButton_->setObjectName(QStringLiteral("quadrantClearButton"));
    hintLabel_ = new QLabel(QStringLiteral("单击替换，Ctrl+单击加入"), this);
    hintLabel_->setObjectName(QStringLiteral("quadrantSelectionHint"));
    hintLabel_->setAlignment(Qt::AlignCenter);
    hintLabel_->setStyleSheet(QStringLiteral("QLabel { color: #666; }"));

    connect(selectAllButton_, &QPushButton::clicked,
            this, &QuadrantSelectionWidget::selectAllRequested);
    connect(clearButton_, &QPushButton::clicked,
            this, &QuadrantSelectionWidget::clearRequested);
    selectAllButton_->setEnabled(false);
    clearButton_->setEnabled(false);
}

QuadrantSelectionWidget::~QuadrantSelectionWidget() {
    if (thumbnailToken_) thumbnailToken_->cancel();
}

QSize QuadrantSelectionWidget::sizeHint() const { return {240, 210}; }

bool QuadrantSelectionWidget::hasGrid() const noexcept {
    return document_ && segmentation_.rows > 0 && segmentation_.columns > 0 &&
           segmentation_.settings.cellWidth > 0 && segmentation_.settings.cellHeight > 0;
}

void QuadrantSelectionWidget::setSource(
        const ImageDocumentPtr& document, const SegmentationResult& segmentation) {
    const bool documentChanged = !document_ || !document ||
                                 document_->generation != document->generation;
    document_ = document;
    segmentation_ = segmentation;
    selectedCells_.clear();
    hoveredQuadrant_ = -1;
    setEnabled(hasGrid());
    selectAllButton_->setEnabled(hasGrid());
    clearButton_->setEnabled(hasGrid());
    Q_EMIT selectionPreviewChanged({});
    if (documentChanged) startThumbnailLoad();
    update();
}

void QuadrantSelectionWidget::setSelectedCells(const QVector<QPoint>& cells) {
    QSet<QPoint> next;
    for (const QPoint& cell : cells) {
        if (cell.x() >= 0 && cell.y() >= 0 &&
            cell.x() < segmentation_.columns && cell.y() < segmentation_.rows) {
            next.insert(cell);
        }
    }
    if (selectedCells_ == next) return;
    selectedCells_ = std::move(next);
    update();
}

void QuadrantSelectionWidget::clear() { setSource({}, {}); }

QRect QuadrantSelectionWidget::cellBoundsForQuadrant(
        const SegmentationResult& segmentation, ImageQuadrant quadrant) {
    if (segmentation.rows <= 0 || segmentation.columns <= 0) return {};
    const int topRows = (segmentation.rows + 1) / 2;
    const int leftColumns = (segmentation.columns + 1) / 2;
    const bool right = quadrant == ImageQuadrant::TopRight ||
                       quadrant == ImageQuadrant::BottomRight;
    const bool bottom = quadrant == ImageQuadrant::BottomLeft ||
                        quadrant == ImageQuadrant::BottomRight;
    const int x = right ? leftColumns : 0;
    const int y = bottom ? topRows : 0;
    const int width = right ? segmentation.columns - leftColumns : leftColumns;
    const int height = bottom ? segmentation.rows - topRows : topRows;
    return width > 0 && height > 0 ? QRect(x, y, width, height) : QRect{};
}

QVector<QPoint> QuadrantSelectionWidget::cellsForQuadrant(
        const SegmentationResult& segmentation, ImageQuadrant quadrant) {
    QVector<QPoint> cells;
    const QRect bounds = cellBoundsForQuadrant(segmentation, quadrant);
    if (bounds.isEmpty()) return cells;
    const qint64 count = qint64(bounds.width()) * bounds.height();
    if (count <= std::numeric_limits<qsizetype>::max())
        cells.reserve(static_cast<qsizetype>(count));
    for (int row = bounds.top(); row <= bounds.bottom(); ++row)
        for (int column = bounds.left(); column <= bounds.right(); ++column)
            cells.push_back({column, row});
    return cells;
}

QVector<QPoint> QuadrantSelectionWidget::allCells(
        const SegmentationResult& segmentation) {
    QVector<QPoint> cells;
    if (segmentation.rows <= 0 || segmentation.columns <= 0) return cells;
    const qint64 count = qint64(segmentation.rows) * segmentation.columns;
    if (count <= std::numeric_limits<qsizetype>::max())
        cells.reserve(static_cast<qsizetype>(count));
    for (int row = 0; row < segmentation.rows; ++row)
        for (int column = 0; column < segmentation.columns; ++column)
            cells.push_back({column, row});
    return cells;
}

bool QuadrantSelectionWidget::isQuadrantSelected(ImageQuadrant quadrant) const {
    const QRect bounds = cellBoundsForQuadrant(segmentation_, quadrant);
    if (bounds.isEmpty()) return false;
    for (int row = bounds.top(); row <= bounds.bottom(); ++row)
        for (int column = bounds.left(); column <= bounds.right(); ++column)
            if (!selectedCells_.contains({column, row})) return false;
    return true;
}

QRect QuadrantSelectionWidget::canvasRect() const {
    return rect().adjusted(6, 6, -6, -kBottomControlsHeight);
}

QRectF QuadrantSelectionWidget::imageRect() const {
    QRectF available = canvasRect();
    if (available.isEmpty()) return {};
    qreal sourceWidth = document_ ? document_->metadata.width : 4.0;
    qreal sourceHeight = document_ ? document_->metadata.height : 3.0;
    if (sourceWidth <= 0.0 || sourceHeight <= 0.0) return available;
    const qreal scale = std::min(available.width() / sourceWidth,
                                 available.height() / sourceHeight);
    const QSizeF fitted(sourceWidth * scale, sourceHeight * scale);
    return QRectF(available.center().x() - fitted.width() / 2.0,
                  available.center().y() - fitted.height() / 2.0,
                  fitted.width(), fitted.height());
}

QPointF QuadrantSelectionWidget::splitPoint(const QRectF& image) const {
    if (!hasGrid()) return image.center();
    const int leftColumns = (segmentation_.columns + 1) / 2;
    const int topRows = (segmentation_.rows + 1) / 2;
    const double splitPixelX = std::min<double>(
        document_->metadata.width, qint64(leftColumns) * segmentation_.settings.cellWidth);
    const double splitPixelY = std::min<double>(
        document_->metadata.height, qint64(topRows) * segmentation_.settings.cellHeight);
    return {image.left() + image.width() * splitPixelX / document_->metadata.width,
            image.top() + image.height() * splitPixelY / document_->metadata.height};
}

ImageQuadrant QuadrantSelectionWidget::quadrantAt(const QPointF& position) const {
    const QRectF image = imageRect();
    const QPointF split = splitPoint(image);
    const bool right = position.x() >= split.x();
    const bool bottom = position.y() >= split.y();
    if (bottom) return right ? ImageQuadrant::BottomRight : ImageQuadrant::BottomLeft;
    return right ? ImageQuadrant::TopRight : ImageQuadrant::TopLeft;
}

QRectF QuadrantSelectionWidget::quadrantRect(
        const QRectF& image, ImageQuadrant quadrant) const {
    const QPointF split = splitPoint(image);
    switch (quadrant) {
    case ImageQuadrant::TopLeft:
        return {image.left(), image.top(), split.x() - image.left(),
                split.y() - image.top()};
    case ImageQuadrant::TopRight:
        return {split.x(), image.top(), image.right() - split.x(),
                split.y() - image.top()};
    case ImageQuadrant::BottomLeft:
        return {image.left(), split.y(), split.x() - image.left(),
                image.bottom() - split.y()};
    case ImageQuadrant::BottomRight:
        return {split.x(), split.y(), image.right() - split.x(),
                image.bottom() - split.y()};
    }
    return {};
}

void QuadrantSelectionWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF image = imageRect();
    painter.fillRect(canvasRect(), palette().window().color().darker(108));
    if (!thumbnail_.isNull()) painter.drawImage(image, thumbnail_);
    else painter.fillRect(image, QColor(52, 56, 61));

    if (!hasGrid()) {
        painter.setPen(QColor(220, 220, 220));
        painter.drawText(image, Qt::AlignCenter, QStringLiteral("生成分割网格后可快速选择"));
        painter.setPen(QColor(120, 120, 120));
        painter.drawRect(image);
        return;
    }

    const QPointF split = splitPoint(image);
    painter.setPen(QPen(QColor(255, 255, 255, 220), 1.5));
    painter.drawLine(QPointF(split.x(), image.top()), QPointF(split.x(), image.bottom()));
    painter.drawLine(QPointF(image.left(), split.y()), QPointF(image.right(), split.y()));
    painter.drawRect(image);

    for (int value = 0; value < 4; ++value) {
        const auto quadrant = static_cast<ImageQuadrant>(value);
        const QRect bounds = cellBoundsForQuadrant(segmentation_, quadrant);
        const QRectF area = quadrantRect(image, quadrant);
        if (area.width() <= 0.0 || area.height() <= 0.0 || bounds.isEmpty()) continue;
        if (isQuadrantSelected(quadrant)) {
            painter.fillRect(area, QColor(255, 200, 0, 90));
            painter.setPen(QPen(QColor(255, 210, 35, 245), 2.5));
            painter.drawRect(area.adjusted(1, 1, -1, -1));
        }
        if (value == hoveredQuadrant_) {
            painter.fillRect(area, QColor(40, 150, 255, 95));
            painter.setPen(QPen(QColor(90, 190, 255), 2.0));
            painter.drawRect(area.adjusted(1, 1, -1, -1));
        }
        const qint64 count = qint64(bounds.width()) * bounds.height();
        const QString label = QStringLiteral("%1\n%2 块")
                                  .arg(quadrantName(quadrant)).arg(count);
        const QRectF labelArea = area.adjusted(3, 3, -3, -3);
        painter.setPen(QColor(0, 0, 0, 210));
        painter.drawText(labelArea.translated(1, 1), Qt::AlignCenter, label);
        painter.setPen(Qt::white);
        painter.drawText(labelArea, Qt::AlignCenter, label);
    }
}

void QuadrantSelectionWidget::resizeEvent(QResizeEvent*) {
    constexpr int margin = 6;
    constexpr int spacing = 4;
    constexpr int buttonHeight = 26;
    const int y = height() - kBottomControlsHeight + 2;
    const int buttonWidth = std::max(1, (width() - margin * 2 - spacing) / 2);
    selectAllButton_->setGeometry(margin, y, buttonWidth, buttonHeight);
    clearButton_->setGeometry(margin + buttonWidth + spacing, y,
                              buttonWidth, buttonHeight);
    hintLabel_->setGeometry(margin, y + buttonHeight + 2,
                            width() - margin * 2, 22);
}

void QuadrantSelectionWidget::updateHoveredQuadrant(const QPointF& position) {
    int next = -1;
    if (isEnabled() && hasGrid() && imageRect().contains(position)) {
        const ImageQuadrant quadrant = quadrantAt(position);
        if (!cellBoundsForQuadrant(segmentation_, quadrant).isEmpty())
            next = static_cast<int>(quadrant);
    }
    if (hoveredQuadrant_ == next) return;
    hoveredQuadrant_ = next;
    Q_EMIT selectionPreviewChanged(
        next < 0 ? QRect{} : cellBoundsForQuadrant(
            segmentation_, static_cast<ImageQuadrant>(next)));
    update();
}

void QuadrantSelectionWidget::mouseMoveEvent(QMouseEvent* event) {
    updateHoveredQuadrant(event->position());
    QWidget::mouseMoveEvent(event);
}

void QuadrantSelectionWidget::mouseReleaseEvent(QMouseEvent* event) {
    updateHoveredQuadrant(event->position());
    if (event->button() == Qt::LeftButton && hoveredQuadrant_ >= 0 &&
        imageRect().contains(event->position())) {
        Q_EMIT quadrantSelected(static_cast<ImageQuadrant>(hoveredQuadrant_),
                                event->modifiers().testFlag(Qt::ControlModifier));
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void QuadrantSelectionWidget::leaveEvent(QEvent* event) {
    if (hoveredQuadrant_ >= 0) {
        hoveredQuadrant_ = -1;
        Q_EMIT selectionPreviewChanged({});
        update();
    }
    QWidget::leaveEvent(event);
}

void QuadrantSelectionWidget::startThumbnailLoad() {
    if (thumbnailToken_) thumbnailToken_->cancel();
    thumbnailToken_ = std::make_shared<CancellationToken>();
    thumbnail_ = {};
    displayedThumbnailGeneration_ = 0;
    const quint64 serial = ++thumbnailSerial_;
    const auto document = document_;
    const auto token = thumbnailToken_;
    if (!document) return;

    auto* watcher = new QFutureWatcher<ThumbnailResult>(this);
    connect(watcher, &QFutureWatcher<ThumbnailResult>::finished, this,
            [this, watcher, serial] {
        const ThumbnailResult result = watcher->result();
        watcher->deleteLater();
        if (result.serial != serial || serial != thumbnailSerial_ || !document_ ||
            result.generation != document_->generation) return;
        thumbnail_ = result.image;
        displayedThumbnailGeneration_ = result.generation;
        update();
        Q_EMIT thumbnailReady(result.generation);
    });
    watcher->setFuture(QtConcurrent::run([document, token, serial] {
        ThumbnailResult result;
        result.serial = serial;
        result.generation = document->generation;
        const double scale = std::min(
            1.0, double(kThumbnailMaximumSide) /
                     std::max(document->metadata.width, document->metadata.height));
        QString error;
        result.image = VipsSupport::renderRegion(
            document, 0, 0, document->metadata.width, document->metadata.height,
            scale, &error, token);
        return result;
    }));
}

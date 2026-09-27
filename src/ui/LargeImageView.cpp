#include "ui/LargeImageView.h"
#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"
#include "io/SplitExporter.h"

#include <QApplication>
#include <QFutureWatcher>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace {
constexpr int kTilePixels = 256;
constexpr double kMinimumGroupLabelCellSize = 24.0;

std::optional<quint32> grayValue(const QImage& image, int x, int y, PixelKind kind) {
    if (image.isNull() || x < 0 || y < 0 || x >= image.width() || y >= image.height())
        return std::nullopt;
    if (image.format() == QImage::Format_Grayscale16)
        return reinterpret_cast<const quint16*>(image.constScanLine(y))[x];
    if (image.depth() < 8) return std::nullopt;
    quint32 value = image.constScanLine(y)[x];
    if (kind == PixelKind::Binary1) value = value == 0 ? 0U : 1U;
    return value;
}
}

LargeImageView::LargeImageView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    tileRequestToken_ = std::make_shared<CancellationToken>();
}

void LargeImageView::setDocument(const ImageDocumentPtr& document) {
    if (tileRequestToken_) tileRequestToken_->cancel();
    tileRequestToken_ = std::make_shared<CancellationToken>();
    document_ = document; cache_.clear(); pending_.clear(); failedTiles_.clear(); visibleTileKeys_.clear(); showGrid_ = false; grouping_ = {};
    probeCache_ = {}; probeCacheOrigin_ = {-1, -1}; hoverPixel_ = {-1, -1}; probePending_ = false; ++probeRequestSerial_;
    const bool hadFixedPoints = !fixedGrayPoints_.isEmpty();
    fixedGrayPoints_.clear(); selectedFixedGrayPointId_ = 0;
    if (hadFixedPoints) Q_EMIT fixedGrayPointCountChanged(0);
    Q_EMIT pixelValueChanged(-1, -1, 0, false);
    showTraversalPath_ = false; clearSelectedCell();
    fitToWindow(); update();
}

void LargeImageView::setSegmentation(const SegmentationResult& segmentation) {
    segmentation_ = segmentation; showGrid_ = segmentation.rows > 0 && segmentation.columns > 0;
    grouping_ = {};
    if (!showGrid_) showTraversalPath_ = false;
    clearSelectedCell(); update();
}

void LargeImageView::clearSegmentation() {
    showGrid_ = false; showTraversalPath_ = false; grouping_ = {}; clearSelectedCell(); update();
}

void LargeImageView::setElementGrouping(const ElementGroupingResult& grouping) {
    grouping_ = grouping.matches(document_, segmentation_) ? grouping : ElementGroupingResult{};
    update();
}

void LargeImageView::clearElementGrouping() {
    grouping_ = {};
    update();
}

void LargeImageView::setTraversalMode(TraversalMode mode) {
    if (traversalMode_ == mode) return;
    traversalMode_ = mode;
    if (showTraversalPath_) update();
    if (!selectedCells_.isEmpty()) Q_EMIT segmentationCellsSelected(selectedCells(), selectedCell_);
}

void LargeImageView::setTraversalPathVisible(bool visible) {
    const bool nextVisible = visible && showGrid_;
    if (showTraversalPath_ == nextVisible) return;
    showTraversalPath_ = nextVisible;
    update();
}

void LargeImageView::setSelectedCell(int row, int column, bool centerOnSelection) {
    if (!document_ || !showGrid_) { clearSelectedCell(); update(); return; }
    const ImageRegion region = segmentationCellRegion(
        segmentation_, document_->metadata.width, document_->metadata.height, row, column);
    if (!region.isValid()) { clearSelectedCell(); update(); return; }
    setSingleSelectedCell({column, row}, false);
    if (centerOnSelection) centerOnCell(row, column);
    update();
}

QVector<QPoint> LargeImageView::selectedCells() const {
    QVector<QPoint> result;
    result.reserve(selectedCells_.size());
    for (const QPoint& cell : selectedCells_) result.push_back(cell);
    std::sort(result.begin(), result.end(), [this](const QPoint& left, const QPoint& right) {
        const qint64 leftIndex = SplitExporter::traversalIndex(
            segmentation_.rows, segmentation_.columns, traversalMode_, left.y(), left.x());
        const qint64 rightIndex = SplitExporter::traversalIndex(
            segmentation_.rows, segmentation_.columns, traversalMode_, right.y(), right.x());
        return leftIndex < rightIndex;
    });
    return result;
}

void LargeImageView::centerOnCell(int row, int column) {
    if (!document_ || zoom_ <= 0.0) return;
    const ImageRegion region = segmentationCellRegion(
        segmentation_, document_->metadata.width, document_->metadata.height, row, column);
    if (!region.isValid()) return;
    const QPointF center(region.x + region.width / 2.0, region.y + region.height / 2.0);
    offset_ = QPointF(width() / 2.0, height() / 2.0) - center * zoom_;
    update();
}

void LargeImageView::fitToWindow() {
    if (!document_ || width() <= 0 || height() <= 0) return;
    zoom_ = std::clamp(std::min(double(width()-20) / document_->metadata.width,
                               double(height()-20) / document_->metadata.height), 0.00001, 64.0);
    centerImage(); emitStatus(); update();
}

void LargeImageView::actualSize() { if(document_) { zoom_=1.0; centerImage(); emitStatus(); update(); } }

void LargeImageView::centerImage() {
    if (!document_) return;
    offset_ = QPointF((width() - document_->metadata.width * zoom_) / 2.0,
                      (height() - document_->metadata.height * zoom_) / 2.0);
}

void LargeImageView::emitStatus() {
    if(document_) Q_EMIT viewStatusChanged(zoom_, document_->metadata.width, document_->metadata.height);
}

QString LargeImageView::tileKey(int level, int tx, int ty) const {
    return QStringLiteral("%1:%2:%3:%4").arg(document_ ? document_->generation : 0).arg(level).arg(tx).arg(ty);
}

void LargeImageView::requestTile(const QString& key, int level, int tx, int ty, int sourceTile) {
    if (!document_ || pending_.contains(key) || failedTiles_.contains(key)) return;
    pending_.insert(key);
    const auto doc = document_; const quint64 generation = doc->generation;
    const auto token = tileRequestToken_;
    const double scale = std::pow(2.0, -level);
    auto* watcher = new QFutureWatcher<QPair<QImage,QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<QImage,QString>>::finished, this, [this, watcher, key, generation] {
        const auto result = watcher->result(); watcher->deleteLater(); pending_.remove(key);
        if (document_ && document_->generation == generation && !result.first.isNull()) {
            auto* image = new QImage(result.first);
            cache_.insert(key, image, std::max(1, int(image->sizeInBytes()/1024)));
        } else if (document_ && document_->generation == generation) {
            const QString message = result.second.isEmpty()
                                        ? QStringLiteral("图块渲染返回了空图像。") : result.second;
            failedTiles_.insert(key, message);
            Q_EMIT tileError(message);
        }
        emitTileProgress();
        update();
    });
    watcher->setFuture(QtConcurrent::run([doc, token, tx, ty, sourceTile, scale] {
        QString error;
        QImage image = VipsSupport::renderRegion(doc, qint64(tx)*sourceTile, qint64(ty)*sourceTile,
                                                 sourceTile, sourceTile, scale, &error, token);
        return qMakePair(image,error);
    }));
}

void LargeImageView::paintEvent(QPaintEvent*) {
    QPainter p(this); p.fillRect(rect(), QColor(38,40,43));
    if (!document_) { p.setPen(Qt::lightGray); p.drawText(rect(), Qt::AlignCenter, QStringLiteral("打开图片或目录开始")); return; }
    const QRectF imageRect(offset_.x(), offset_.y(),
                           document_->metadata.width * zoom_,
                           document_->metadata.height * zoom_);
    const QRectF visibleImageRect = imageRect.intersected(QRectF(rect()));
    if (visibleImageRect.isEmpty()) return;
    const int level = zoom_ >= 1.0 ? 0 : std::max(0, int(std::floor(-std::log2(zoom_))));
    const int sourceTile = kTilePixels * (1 << std::min(level,20));
    const double targetTile = sourceTile * zoom_;
    const double leftImage = std::max(0.0, -offset_.x()/zoom_);
    const double topImage = std::max(0.0, -offset_.y()/zoom_);
    const double rightImage = std::min(double(document_->metadata.width), (width()-offset_.x())/zoom_);
    const double bottomImage = std::min(double(document_->metadata.height), (height()-offset_.y())/zoom_);
    const int tx0=std::max(0,int(std::floor(leftImage/sourceTile))), ty0=std::max(0,int(std::floor(topImage/sourceTile)));
    const int tx1=std::max(tx0,int(std::ceil(rightImage/sourceTile))), ty1=std::max(ty0,int(std::ceil(bottomImage/sourceTile)));
    QSet<QString> visibleTileKeys;
    for(int ty=ty0;ty<ty1;++ty) for(int tx=tx0;tx<tx1;++tx) {
        const QString key=tileKey(level,tx,ty);
        const qint64 sourceX = qint64(tx) * sourceTile;
        const qint64 sourceY = qint64(ty) * sourceTile;
        const qint64 actualSourceWidth = std::min<qint64>(sourceTile, document_->metadata.width - sourceX);
        const qint64 actualSourceHeight = std::min<qint64>(sourceTile, document_->metadata.height - sourceY);
        if (actualSourceWidth <= 0 || actualSourceHeight <= 0) continue;
        visibleTileKeys.insert(key);
        const QRectF target(offset_.x()+tx*targetTile, offset_.y()+ty*targetTile,
                            actualSourceWidth*zoom_, actualSourceHeight*zoom_);
        if(QImage* image=cache_.object(key)) { p.drawImage(target,*image); }
        else if (!failedTiles_.contains(key)) {
            p.fillRect(target,QColor(55,58,61));
            requestTile(key,level,tx,ty,sourceTile);
        }
    }
    visibleTileKeys_=std::move(visibleTileKeys);
    emitTileProgress();

    if(showGrid_) {
        p.save();
        const double patternW=segmentation_.settings.cellWidth*zoom_;
        const double patternH=segmentation_.settings.cellHeight*zoom_;
        const double pitchX=patternW;
        const double pitchY=patternH;
        const double baseX=offset_.x();
        const double baseY=offset_.y();
        p.setClipRect(visibleImageRect);

        // Fixed Rect2D cells tile continuously from image coordinate (0,0).
        const double minimumSpacing = std::min(pitchX, pitchY);
        const int alpha = minimumSpacing >= 16.0 ? 190 : minimumSpacing >= 8.0 ? 115 : 70;
        const bool hasGrouping = grouping_.matches(document_, segmentation_);
        const int firstVisibleColumn=std::max(0,int(std::floor(leftImage/segmentation_.settings.cellWidth)));
        const int lastVisibleColumn=std::min(segmentation_.columns-1,int(std::floor(std::max(0.0,rightImage-1.0)/segmentation_.settings.cellWidth)));
        const int firstVisibleRow=std::max(0,int(std::floor(topImage/segmentation_.settings.cellHeight)));
        const int lastVisibleRow=std::min(segmentation_.rows-1,int(std::floor(std::max(0.0,bottomImage-1.0)/segmentation_.settings.cellHeight)));
        if (hasGrouping && minimumSpacing >= 2.0) {
            p.setPen(Qt::NoPen);
            for(int r=firstVisibleRow;r<=lastVisibleRow;++r)for(int c=firstVisibleColumn;c<=lastVisibleColumn;++c) {
                const ImageRegion cell=segmentationCellRegion(
                    segmentation_,document_->metadata.width,document_->metadata.height,r,c);
                QColor color=grouping_.colorAt(r,c);
                if(!cell.isValid()||!color.isValid())continue;
                color.setAlpha(72);
                p.fillRect(QRectF(baseX+cell.x*zoom_,baseY+cell.y*zoom_,
                                  cell.width*zoom_,cell.height*zoom_),color);
            }
        }
        p.setPen(QPen(QColor(255,70,70,alpha),1));
        if(minimumSpacing>=4.0) {
            const int firstColumn=std::max(0,int(std::ceil((visibleImageRect.left()-baseX-patternW)/pitchX)));
            const int lastColumn=std::min(segmentation_.columns-1,int(std::floor((visibleImageRect.right()-baseX)/pitchX)));
            const int firstRow=std::max(0,int(std::ceil((visibleImageRect.top()-baseY-patternH)/pitchY)));
            const int lastRow=std::min(segmentation_.rows-1,int(std::floor((visibleImageRect.bottom()-baseY)/pitchY)));
            for(int r=firstRow;r<=lastRow;++r) {
                for(int c=firstColumn;c<=lastColumn;++c) {
                    const ImageRegion cell = segmentationCellRegion(
                        segmentation_, document_->metadata.width, document_->metadata.height, r, c);
                    if (!cell.isValid()) continue;
                    p.drawRect(QRectF(baseX + cell.x * zoom_, baseY + cell.y * zoom_,
                                      cell.width * zoom_, cell.height * zoom_));
                }
            }
        }

        if(showTraversalPath_ && minimumSpacing>=2.0) {
            p.setPen(QPen(QColor(0,190,255,220),2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
            const int firstColumn=std::max(0,int(std::floor(leftImage/segmentation_.settings.cellWidth))-1);
            const int lastColumn=std::min(segmentation_.columns-1,int(std::floor(rightImage/segmentation_.settings.cellWidth))+1);
            const int firstRow=std::max(0,int(std::floor(topImage/segmentation_.settings.cellHeight))-1);
            const int lastRow=std::min(segmentation_.rows-1,int(std::floor(bottomImage/segmentation_.settings.cellHeight))+1);
            for(int r=firstRow;r<=lastRow;++r)for(int c=firstColumn;c<=lastColumn;++c) {
                const QPoint next=SplitExporter::nextTraversalCell(
                    segmentation_.rows,segmentation_.columns,traversalMode_,r,c);
                if(next.x()<0||next.y()<0)continue;
                const ImageRegion currentRegion=segmentationCellRegion(
                    segmentation_,document_->metadata.width,document_->metadata.height,r,c);
                const ImageRegion nextRegion=segmentationCellRegion(
                    segmentation_,document_->metadata.width,document_->metadata.height,next.y(),next.x());
                if(!currentRegion.isValid()||!nextRegion.isValid())continue;
                const QPointF currentCenter(baseX+(currentRegion.x+currentRegion.width/2.0)*zoom_,
                                            baseY+(currentRegion.y+currentRegion.height/2.0)*zoom_);
                const QPointF nextCenter(baseX+(nextRegion.x+nextRegion.width/2.0)*zoom_,
                                         baseY+(nextRegion.y+nextRegion.height/2.0)*zoom_);
                p.drawLine(currentCenter,nextCenter);
            }
        }

        if (hasGrouping && minimumSpacing >= kMinimumGroupLabelCellSize) {
            p.setRenderHint(QPainter::TextAntialiasing, true);
            QFont labelFont=p.font();
            labelFont.setBold(true);
            labelFont.setPixelSize(std::clamp(int(std::floor(minimumSpacing*0.30)),10,28));
            p.setFont(labelFont);
            const QFontMetricsF metrics(labelFont);
            QHash<int,QSizeF> badgeSizes;
            for(int r=firstVisibleRow;r<=lastVisibleRow;++r)for(int c=firstVisibleColumn;c<=lastVisibleColumn;++c) {
                const int groupId=grouping_.groupIdAt(r,c);
                const bool background=grouping_.isBackgroundAt(r,c);
                if(groupId<0&&!background)continue;
                const ImageRegion cell=segmentationCellRegion(
                    segmentation_,document_->metadata.width,document_->metadata.height,r,c);
                if(!cell.isValid())continue;
                const QRectF cellRect(baseX+cell.x*zoom_,baseY+cell.y*zoom_,
                                      cell.width*zoom_,cell.height*zoom_);
                const QString label=background?QStringLiteral("x"):QString::number(groupId+1);
                auto badgeIt=badgeSizes.constFind(groupId);
                QSizeF badgeSize;
                if(badgeIt==badgeSizes.cend()) {
                    badgeSize=QSizeF(std::ceil(metrics.horizontalAdvance(label))+12.0,
                                    std::ceil(metrics.height())+6.0);
                    badgeSizes.insert(groupId,badgeSize);
                } else {
                    badgeSize=*badgeIt;
                }
                const QRectF available=cellRect.adjusted(3.0,3.0,-3.0,-3.0);
                if(available.width()<badgeSize.width()||available.height()<badgeSize.height())continue;
                const QRectF badge(available.center().x()-badgeSize.width()/2.0,
                                   available.center().y()-badgeSize.height()/2.0,
                                   badgeSize.width(),badgeSize.height());
                p.setPen(QPen(QColor(255,255,255,190),1.0));
                p.setBrush(QColor(15,18,22,215));
                p.drawRoundedRect(badge,4.0,4.0);
                p.setPen(Qt::white);
                p.drawText(badge,Qt::AlignCenter,label);
            }
        }

        for (const QPoint& selected : selectedCells_) {
            const ImageRegion selectedRegion=segmentationCellRegion(
                segmentation_,document_->metadata.width,document_->metadata.height,
                selected.y(),selected.x());
            if(selectedRegion.isValid()) {
                const QRectF selectedRect(baseX+selectedRegion.x*zoom_,baseY+selectedRegion.y*zoom_,
                                          selectedRegion.width*zoom_,selectedRegion.height*zoom_);
                p.fillRect(selectedRect,QColor(255,200,0,80));
                const bool active = selected == selectedCell_;
                p.setPen(QPen(active ? QColor(255,235,40,255) : QColor(255,190,0,210),
                              active ? 3.0 : 1.5));
                p.drawRect(selectedRect);
            }
        }
        p.restore();
    }
    drawFixedGrayPoints(p);
    if (boxSelecting_) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(boxSelectionRect_, QColor(40, 150, 255, 55));
        p.setPen(QPen(QColor(70, 175, 255, 245), 1.5, Qt::DashLine));
        p.drawRect(boxSelectionRect_);
        p.restore();
    }
}

void LargeImageView::emitTileProgress() {
    if (!document_ || visibleTileKeys_.isEmpty()) return;
    qint64 completed = 0;
    qint64 failed = 0;
    for (const QString& key : std::as_const(visibleTileKeys_)) {
        if (cache_.contains(key)) ++completed;
        else if (failedTiles_.contains(key)) ++failed;
    }
    Q_EMIT tileProgress(document_->generation, completed, failed, visibleTileKeys_.size());
}

void LargeImageView::resizeEvent(QResizeEvent*) { update(); }
void LargeImageView::mousePressEvent(QMouseEvent* e) {
    if(e->button()!=Qt::LeftButton)return;
    setFocus(Qt::MouseFocusReason);
    leftPressed_=true;dragging_=false;pressModifiers_=e->modifiers();
    pressMultiSelect_=multiSelectHeld_;
    pressBoxSelect_=boxSelectHeld_;
    if (pressBoxSelect_) {
        boxSelecting_ = true;
        boxSelectionStart_ = e->pos();
        boxSelectionRect_ = QRect(boxSelectionStart_, boxSelectionStart_);
        setCursor(Qt::CrossCursor);
    }
    pressPosition_=lastMouse_=e->pos();e->accept();
}
void LargeImageView::mouseMoveEvent(QMouseEvent* e) {
    updatePixelProbe(e->pos());
    if(!leftPressed_)return;
    if (pressBoxSelect_) {
        boxSelectionRect_ = QRect(boxSelectionStart_, e->pos()).normalized();
        update();
        e->accept();
        return;
    }
    if(!dragging_&&(e->pos()-pressPosition_).manhattanLength()<QApplication::startDragDistance())return;
    if(!dragging_){dragging_=true;setCursor(Qt::ClosedHandCursor);}
    offset_+=e->pos()-lastMouse_;lastMouse_=e->pos();update();e->accept();
}

void LargeImageView::leaveEvent(QEvent* event) {
    hoverPixel_ = {-1, -1};
    Q_EMIT pixelValueChanged(-1, -1, 0, false);
    QWidget::leaveEvent(event);
}

void LargeImageView::updatePixelProbe(const QPoint& viewportPosition) {
    if (!document_ || zoom_ <= 0.0 || !ImageLoader::isProcessable(document_->metadata)) {
        if (hoverPixel_.x() >= 0) Q_EMIT pixelValueChanged(-1, -1, 0, false);
        hoverPixel_ = {-1, -1};
        return;
    }
    const QPointF imagePosition = (QPointF(viewportPosition) - offset_) / zoom_;
    const qint64 x = static_cast<qint64>(std::floor(imagePosition.x()));
    const qint64 y = static_cast<qint64>(std::floor(imagePosition.y()));
    if (x < 0 || y < 0 || x >= document_->metadata.width || y >= document_->metadata.height) {
        if (hoverPixel_.x() >= 0) Q_EMIT pixelValueChanged(-1, -1, 0, false);
        hoverPixel_ = {-1, -1};
        return;
    }
    hoverPixel_ = {static_cast<int>(x), static_cast<int>(y)};
    if (!probeCache_.isNull() && x >= probeCacheOrigin_.x() && y >= probeCacheOrigin_.y() &&
        x < probeCacheOrigin_.x() + probeCache_.width() &&
        y < probeCacheOrigin_.y() + probeCache_.height()) {
        emitCachedProbe();
    } else if (!probePending_) {
        requestProbeBlock(x, y);
    }
}

void LargeImageView::requestProbeBlock(qint64 x, qint64 y) {
    if (!document_ || probePending_) return;
    constexpr qint64 blockSize = 64;
    const qint64 blockX = (x / blockSize) * blockSize;
    const qint64 blockY = (y / blockSize) * blockSize;
    const qint64 blockWidth = std::min(blockSize, document_->metadata.width - blockX);
    const qint64 blockHeight = std::min(blockSize, document_->metadata.height - blockY);
    const auto doc = document_;
    const quint64 generation = doc->generation;
    const quint64 requestSerial = ++probeRequestSerial_;
    probePending_ = true;
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, generation, requestSerial, blockX, blockY] {
        const QImage image = watcher->result();
        watcher->deleteLater();
        if (requestSerial != probeRequestSerial_) return;
        probePending_ = false;
        if (!document_ || document_->generation != generation) return;
        if (!image.isNull()) {
            probeCache_ = image;
            probeCacheOrigin_ = {static_cast<int>(blockX), static_cast<int>(blockY)};
        }
        if (hoverPixel_.x() < 0) return;
        if (!probeCache_.isNull() && hoverPixel_.x() >= probeCacheOrigin_.x() &&
            hoverPixel_.y() >= probeCacheOrigin_.y() &&
            hoverPixel_.x() < probeCacheOrigin_.x() + probeCache_.width() &&
            hoverPixel_.y() < probeCacheOrigin_.y() + probeCache_.height()) {
            emitCachedProbe();
        } else {
            requestProbeBlock(hoverPixel_.x(), hoverPixel_.y());
        }
    });
    watcher->setFuture(QtConcurrent::run([doc, blockX, blockY, blockWidth, blockHeight] {
        QString error;
        return VipsSupport::renderRegion(doc, blockX, blockY, blockWidth, blockHeight, 1.0, &error);
    }));
}

void LargeImageView::emitCachedProbe() {
    if (!document_ || probeCache_.isNull() || hoverPixel_.x() < 0) return;
    const int localX = hoverPixel_.x() - probeCacheOrigin_.x();
    const int localY = hoverPixel_.y() - probeCacheOrigin_.y();
    const auto value = grayValue(probeCache_, localX, localY, document_->metadata.kind);
    if (value) Q_EMIT pixelValueChanged(hoverPixel_.x(), hoverPixel_.y(), *value, true);
}

void LargeImageView::addFixedGrayPointAt(const QPoint& viewportPosition) {
    if (!document_ || zoom_ <= 0.0 || !ImageLoader::isProcessable(document_->metadata)) return;
    if (const quint64 existingId = fixedGrayPointAt(viewportPosition); existingId != 0) {
        selectedFixedGrayPointId_ = existingId;
        update();
        return;
    }
    const QPointF imagePosition = (QPointF(viewportPosition) - offset_) / zoom_;
    const qint64 x = static_cast<qint64>(std::floor(imagePosition.x()));
    const qint64 y = static_cast<qint64>(std::floor(imagePosition.y()));
    if (x < 0 || y < 0 || x >= document_->metadata.width || y >= document_->metadata.height) return;
    const auto duplicate = std::find_if(fixedGrayPoints_.cbegin(), fixedGrayPoints_.cend(),
        [x, y](const FixedGrayPoint& point) { return point.x == x && point.y == y; });
    if (duplicate != fixedGrayPoints_.cend()) {
        selectedFixedGrayPointId_ = duplicate->id;
        update();
        return;
    }

    FixedGrayPoint point;
    point.id = nextFixedGrayPointId_++;
    point.x = x;
    point.y = y;
    selectedFixedGrayPointId_ = point.id;
    fixedGrayPoints_.push_back(point);
    Q_EMIT fixedGrayPointCountChanged(fixedGrayPoints_.size());
    update();

    if (!probeCache_.isNull() && x >= probeCacheOrigin_.x() && y >= probeCacheOrigin_.y() &&
        x < probeCacheOrigin_.x() + probeCache_.width() &&
        y < probeCacheOrigin_.y() + probeCache_.height()) {
        const auto value = grayValue(probeCache_, static_cast<int>(x - probeCacheOrigin_.x()),
                                     static_cast<int>(y - probeCacheOrigin_.y()),
                                     document_->metadata.kind);
        if (value) {
            fixedGrayPoints_.back().value = *value;
            fixedGrayPoints_.back().state = FixedGrayPointState::Ready;
            update();
            return;
        }
    }

    const auto doc = document_;
    const quint64 generation = doc->generation;
    const quint64 pointId = point.id;
    auto* watcher = new QFutureWatcher<QPair<QImage, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<QImage, QString>>::finished, this,
            [this, watcher, generation, pointId] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!document_ || document_->generation != generation) return;
        const auto pointIt = std::find_if(fixedGrayPoints_.begin(), fixedGrayPoints_.end(),
            [pointId](const FixedGrayPoint& candidate) { return candidate.id == pointId; });
        if (pointIt == fixedGrayPoints_.end()) return;
        const auto value = grayValue(result.first, 0, 0, document_->metadata.kind);
        if (value) {
            pointIt->value = *value;
            pointIt->state = FixedGrayPointState::Ready;
        } else {
            pointIt->state = FixedGrayPointState::Failed;
            Q_EMIT fixedGrayPointError(result.second.isEmpty()
                ? QStringLiteral("无法读取源像素。") : result.second);
        }
        update();
    });
    watcher->setFuture(QtConcurrent::run([doc, x, y] {
        QString error;
        QImage image = VipsSupport::renderRegion(doc, x, y, 1, 1, 1.0, &error);
        return qMakePair(image, error);
    }));
}

QPointF LargeImageView::fixedGrayPointPosition(const FixedGrayPoint& point) const {
    return offset_ + QPointF((static_cast<double>(point.x) + 0.5) * zoom_,
                             (static_cast<double>(point.y) + 0.5) * zoom_);
}

QString LargeImageView::fixedGrayPointLabel(const FixedGrayPoint& point) const {
    if (point.state == FixedGrayPointState::Ready)
        return QStringLiteral("灰度: %1").arg(point.value);
    if (point.state == FixedGrayPointState::Failed) return QStringLiteral("灰度: 读取失败");
    return QStringLiteral("灰度: …");
}

QRectF LargeImageView::fixedGrayPointLabelRect(const FixedGrayPoint& point) const {
    const QPointF marker = fixedGrayPointPosition(point);
    const QString label = fixedGrayPointLabel(point);
    const QSizeF textSize(fontMetrics().horizontalAdvance(label), fontMetrics().height());
    const QSizeF boxSize(textSize.width() + 10.0, textSize.height() + 6.0);
    QPointF topLeft(marker.x() + 10.0, marker.y() - boxSize.height() - 5.0);
    if (topLeft.x() + boxSize.width() > width() - 2.0)
        topLeft.setX(marker.x() - boxSize.width() - 10.0);
    if (topLeft.y() < 2.0) topLeft.setY(marker.y() + 10.0);
    topLeft.setX(std::clamp(topLeft.x(), 2.0, std::max(2.0, width() - boxSize.width() - 2.0)));
    topLeft.setY(std::clamp(topLeft.y(), 2.0, std::max(2.0, height() - boxSize.height() - 2.0)));
    return {topLeft, boxSize};
}

quint64 LargeImageView::fixedGrayPointAt(const QPoint& viewportPosition) const {
    for (auto it = fixedGrayPoints_.crbegin(); it != fixedGrayPoints_.crend(); ++it) {
        const QPointF marker = fixedGrayPointPosition(*it);
        const QPointF delta = QPointF(viewportPosition) - marker;
        if (delta.x() * delta.x() + delta.y() * delta.y() <= 100.0 ||
            fixedGrayPointLabelRect(*it).adjusted(-2.0, -2.0, 2.0, 2.0).contains(viewportPosition))
            return it->id;
    }
    return 0;
}

void LargeImageView::drawFixedGrayPoints(QPainter& painter) const {
    if (fixedGrayPoints_.isEmpty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const auto& point : fixedGrayPoints_) {
        const QPointF marker = fixedGrayPointPosition(point);
        if (!QRectF(rect()).adjusted(-12.0, -12.0, 12.0, 12.0).contains(marker)) continue;
        const bool selected = point.id == selectedFixedGrayPointId_;
        const QColor accent = selected ? QColor(255, 215, 0) : QColor(0, 220, 255);
        painter.setPen(QPen(QColor(0, 0, 0, 210), 4.0));
        painter.drawEllipse(marker, 6.0, 6.0);
        painter.drawLine(marker + QPointF(-9.0, 0.0), marker + QPointF(9.0, 0.0));
        painter.drawLine(marker + QPointF(0.0, -9.0), marker + QPointF(0.0, 9.0));
        painter.setPen(QPen(accent, 2.0));
        painter.drawEllipse(marker, 6.0, 6.0);
        painter.drawLine(marker + QPointF(-9.0, 0.0), marker + QPointF(9.0, 0.0));
        painter.drawLine(marker + QPointF(0.0, -9.0), marker + QPointF(0.0, 9.0));
        const QRectF labelRect = fixedGrayPointLabelRect(point);
        painter.setPen(QPen(accent, selected ? 2.0 : 1.0));
        painter.setBrush(QColor(15, 18, 22, 220));
        painter.drawRoundedRect(labelRect, 4.0, 4.0);
        painter.setPen(Qt::white);
        painter.drawText(labelRect.adjusted(5.0, 3.0, -5.0, -3.0),
                         Qt::AlignLeft | Qt::AlignVCenter, fixedGrayPointLabel(point));
    }
    painter.restore();
}

void LargeImageView::removeSelectedFixedGrayPoint() {
    const auto pointIt = std::find_if(fixedGrayPoints_.begin(), fixedGrayPoints_.end(),
        [this](const FixedGrayPoint& point) { return point.id == selectedFixedGrayPointId_; });
    if (pointIt == fixedGrayPoints_.end()) return;
    fixedGrayPoints_.erase(pointIt);
    selectedFixedGrayPointId_ = 0;
    Q_EMIT fixedGrayPointCountChanged(fixedGrayPoints_.size());
    update();
}

void LargeImageView::clearFixedGrayPoints() {
    if (fixedGrayPoints_.isEmpty()) return;
    fixedGrayPoints_.clear();
    selectedFixedGrayPointId_ = 0;
    Q_EMIT fixedGrayPointCountChanged(0);
    update();
}

void LargeImageView::mouseReleaseEvent(QMouseEvent* e) {
    if(e->button()!=Qt::LeftButton||!leftPressed_)return;
    leftPressed_=false;
    if (pressBoxSelect_) {
        boxSelectionRect_ = QRect(boxSelectionStart_, e->pos()).normalized();
        selectCellsInRect(boxSelectionRect_, e->pos());
        pressBoxSelect_ = false;
        boxSelecting_ = false;
        if (boxSelectHeld_) setCursor(Qt::CrossCursor); else unsetCursor();
        update();
    }
    else if(dragging_){dragging_=false;unsetCursor();}
    else if (pressMultiSelect_) toggleCellAt(e->pos());
    else if (pressModifiers_ == Qt::ShiftModifier) {
        const QPoint cell = cellAt(e->pos());
        if (cell.x() >= 0 && cell.y() >= 0) {
            if (selectedCells_.contains(cell)) setActiveCell(cell);
            else setSingleSelectedCell(cell);
            update();
        }
    }
    else if (pressModifiers_.testFlag(Qt::ControlModifier)) addFixedGrayPointAt(e->pos());
    else {
        const quint64 fixedPointId = fixedGrayPointAt(e->pos());
        if (fixedPointId != 0) {
            selectedFixedGrayPointId_ = fixedPointId;
            update();
        } else {
            selectedFixedGrayPointId_ = 0;
            selectCellAt(e->pos());
        }
    }
    e->accept();
}

void LargeImageView::mouseDoubleClickEvent(QMouseEvent* event) {
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    if (multiSelectHeld_ || boxSelectHeld_) {
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton ||
        (modifiers != Qt::NoModifier && modifiers != Qt::ShiftModifier)) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const QPoint cell = cellAt(event->pos());
    if (cell.x() < 0 || cell.y() < 0) {
        event->ignore();
        return;
    }
    if (modifiers == Qt::ShiftModifier) {
        if (selectedCells_.contains(cell)) setActiveCell(cell);
        else setSingleSelectedCell(cell);
        if (selectedCells_.size() == 1)
            Q_EMIT segmentationCellSliceActivated(cell.y(), cell.x());
        else
            Q_EMIT segmentationCellsSliceActivated(selectedCells(), selectedCell_);
    } else {
        setSingleSelectedCell(cell);
        Q_EMIT segmentationCellActivated(cell.y(), cell.x());
    }
    update();
    event->accept();
}

void LargeImageView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Q) {
        multiSelectHeld_ = true;
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F && !event->isAutoRepeat()) {
        if (!selectedCells_.isEmpty())
            Q_EMIT segmentationCellsSliceActivated(selectedCells(), selectedCell_);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_W) {
        boxSelectHeld_ = true;
        if (!leftPressed_) setCursor(Qt::CrossCursor);
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
        selectedFixedGrayPointId_ != 0) {
        removeSelectedFixedGrayPoint();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void LargeImageView::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Q) {
        if (!event->isAutoRepeat()) multiSelectHeld_ = false;
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_W) {
        if (!event->isAutoRepeat()) boxSelectHeld_ = false;
        if (!leftPressed_) unsetCursor();
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void LargeImageView::focusOutEvent(QFocusEvent* event) {
    leftPressed_ = false;
    dragging_ = false;
    multiSelectHeld_ = false;
    pressMultiSelect_ = false;
    boxSelectHeld_ = false;
    pressBoxSelect_ = false;
    boxSelecting_ = false;
    unsetCursor();
    QWidget::focusOutEvent(event);
}

bool LargeImageView::event(QEvent* event) {
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide) {
        leftPressed_ = false;
        dragging_ = false;
        multiSelectHeld_ = false;
        pressMultiSelect_ = false;
        boxSelectHeld_ = false;
        pressBoxSelect_ = false;
        boxSelecting_ = false;
        unsetCursor();
    }
    return QWidget::event(event);
}

QPoint LargeImageView::cellAt(const QPoint& viewportPosition) const {
    if(!document_||!showGrid_||zoom_<=0.0)return {-1,-1};
    const QPointF imagePosition=(QPointF(viewportPosition)-offset_)/zoom_;
    if(imagePosition.x()<0.0||imagePosition.y()<0.0||
       imagePosition.x()>=document_->metadata.width||imagePosition.y()>=document_->metadata.height){
        return {-1,-1};
    }
    const int column=int(std::floor(imagePosition.x()/segmentation_.settings.cellWidth));
    const int row=int(std::floor(imagePosition.y()/segmentation_.settings.cellHeight));
    const ImageRegion region=segmentationCellRegion(
        segmentation_,document_->metadata.width,document_->metadata.height,row,column);
    return region.isValid()?QPoint(column,row):QPoint(-1,-1);
}

void LargeImageView::selectCellAt(const QPoint& viewportPosition) {
    const QPoint cell=cellAt(viewportPosition);
    if(cell.x()<0||cell.y()<0){clearSelectedCell();update();return;}
    setSingleSelectedCell(cell);
    update();
}

void LargeImageView::toggleCellAt(const QPoint& viewportPosition) {
    const QPoint cell = cellAt(viewportPosition);
    if (cell.x() < 0 || cell.y() < 0) return;
    if (selectedCells_.contains(cell)) {
        selectedCells_.remove(cell);
        if (selectedCell_ == cell) {
            const QVector<QPoint> remaining = selectedCells();
            selectedCell_ = remaining.isEmpty() ? QPoint(-1, -1) : remaining.front();
        }
    } else {
        selectedCells_.insert(cell);
        selectedCell_ = cell;
    }
    emitSelectionChanged();
    update();
}

void LargeImageView::selectCellsInRect(const QRect& viewportRect,
                                       const QPoint& releasePosition) {
    if (!document_ || !showGrid_) return;
    QRect selection = viewportRect.normalized();
    if (selection.width() < 2 && selection.height() < 2) {
        const QPoint cell = cellAt(releasePosition);
        setSingleSelectedCell(cell);
        return;
    }
    QSet<QPoint> nextSelection;
    const QRectF selectionRect(selection);
    const QRectF imageRect(offset_.x(), offset_.y(),
                           document_->metadata.width * zoom_,
                           document_->metadata.height * zoom_);
    if (!selectionRect.intersects(imageRect)) {
        selectedCells_.clear();
        selectedCell_ = {-1, -1};
        emitSelectionChanged();
        return;
    }
    const double imageLeft = (selectionRect.left() - offset_.x()) / zoom_;
    const double imageRight = (selectionRect.right() - offset_.x()) / zoom_;
    const double imageTop = (selectionRect.top() - offset_.y()) / zoom_;
    const double imageBottom = (selectionRect.bottom() - offset_.y()) / zoom_;
    const int firstColumn = std::clamp(static_cast<int>(std::floor(
        imageLeft / segmentation_.settings.cellWidth)), 0, segmentation_.columns - 1);
    const int lastColumn = std::clamp(static_cast<int>(std::floor(
        imageRight / segmentation_.settings.cellWidth)), 0, segmentation_.columns - 1);
    const int firstRow = std::clamp(static_cast<int>(std::floor(
        imageTop / segmentation_.settings.cellHeight)), 0, segmentation_.rows - 1);
    const int lastRow = std::clamp(static_cast<int>(std::floor(
        imageBottom / segmentation_.settings.cellHeight)), 0, segmentation_.rows - 1);
    for (int row = firstRow; row <= lastRow; ++row) {
        for (int column = firstColumn; column <= lastColumn; ++column) {
            const ImageRegion region = segmentationCellRegion(
                segmentation_, document_->metadata.width, document_->metadata.height,
                row, column);
            if (!region.isValid()) continue;
            const QRectF cellRect(offset_.x() + region.x * zoom_,
                                  offset_.y() + region.y * zoom_,
                                  region.width * zoom_, region.height * zoom_);
            if (selectionRect.intersects(cellRect)) nextSelection.insert({column, row});
        }
    }
    selectedCells_ = std::move(nextSelection);
    const QPoint releasedCell = cellAt(releasePosition);
    if (selectedCells_.contains(releasedCell)) selectedCell_ = releasedCell;
    else {
        const QVector<QPoint> ordered = selectedCells();
        selectedCell_ = ordered.isEmpty() ? QPoint(-1, -1) : ordered.front();
    }
    emitSelectionChanged();
}

void LargeImageView::setSingleSelectedCell(const QPoint& cell, bool notify) {
    selectedCells_.clear();
    if (cell.x() >= 0 && cell.y() >= 0) selectedCells_.insert(cell);
    selectedCell_ = selectedCells_.isEmpty() ? QPoint(-1, -1) : cell;
    if (notify) emitSelectionChanged();
}

void LargeImageView::setActiveCell(const QPoint& cell, bool notify) {
    if (!selectedCells_.contains(cell)) return;
    selectedCell_ = cell;
    if (notify) emitSelectionChanged();
}

void LargeImageView::emitSelectionChanged() {
    Q_EMIT segmentationCellSelected(selectedCell_.y(), selectedCell_.x());
    Q_EMIT segmentationCellsSelected(selectedCells(), selectedCell_);
}

void LargeImageView::clearSelectedCell() {
    if(selectedCell_.x()<0&&selectedCell_.y()<0&&selectedCells_.isEmpty())return;
    selectedCells_.clear();selectedCell_={-1,-1};emitSelectionChanged();
}
void LargeImageView::wheelEvent(QWheelEvent* e) {
    if(!document_)return; const QPointF pos=e->position(); const QPointF imagePos=(pos-offset_)/zoom_;
    zoom_=std::clamp(zoom_*std::pow(1.0015,e->angleDelta().y()),0.00001,64.0);
    offset_=pos-imagePos*zoom_; emitStatus(); update(); e->accept();
}

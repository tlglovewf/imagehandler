#include "ui/LargeImageView.h"
#include "imaging/VipsSupport.h"
#include "io/SplitExporter.h"

#include <QApplication>
#include <QFutureWatcher>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>

namespace { constexpr int kTilePixels = 256; }

LargeImageView::LargeImageView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
}

void LargeImageView::setDocument(const ImageDocumentPtr& document) {
    document_ = document; cache_.clear(); pending_.clear(); failedTiles_.clear(); showGrid_ = false;
    showTraversalPath_ = false; clearSelectedCell();
    fitToWindow(); update();
}

void LargeImageView::setSegmentation(const SegmentationResult& segmentation) {
    segmentation_ = segmentation; showGrid_ = segmentation.rows > 0 && segmentation.columns > 0;
    if (!showGrid_) showTraversalPath_ = false;
    clearSelectedCell(); update();
}

void LargeImageView::clearSegmentation() {
    showGrid_ = false; showTraversalPath_ = false; clearSelectedCell(); update();
}

void LargeImageView::setTraversalMode(TraversalMode mode) {
    if (traversalMode_ == mode) return;
    traversalMode_ = mode;
    if (showTraversalPath_) update();
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
    selectedCell_ = {column, row};
    if (centerOnSelection) centerOnCell(row, column);
    update();
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
        update();
    });
    watcher->setFuture(QtConcurrent::run([doc, tx, ty, sourceTile, scale] {
        QString error;
        QImage image = VipsSupport::renderRegion(doc, qint64(tx)*sourceTile, qint64(ty)*sourceTile,
                                                 sourceTile, sourceTile, scale, &error);
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
    qint64 total=0,completed=0;
    for(int ty=ty0;ty<ty1;++ty) for(int tx=tx0;tx<tx1;++tx) {
        ++total; const QString key=tileKey(level,tx,ty);
        const qint64 sourceX = qint64(tx) * sourceTile;
        const qint64 sourceY = qint64(ty) * sourceTile;
        const qint64 actualSourceWidth = std::min<qint64>(sourceTile, document_->metadata.width - sourceX);
        const qint64 actualSourceHeight = std::min<qint64>(sourceTile, document_->metadata.height - sourceY);
        if (actualSourceWidth <= 0 || actualSourceHeight <= 0) continue;
        const QRectF target(offset_.x()+tx*targetTile, offset_.y()+ty*targetTile,
                            actualSourceWidth*zoom_, actualSourceHeight*zoom_);
        if(QImage* image=cache_.object(key)) { p.drawImage(target,*image); ++completed; }
        else if (failedTiles_.contains(key)) {
            p.fillRect(target, QColor(70, 42, 42));
            p.setPen(QPen(QColor(220, 110, 110), 1));
            p.drawLine(target.topLeft(), target.bottomRight());
            p.drawLine(target.topRight(), target.bottomLeft());
            ++completed;
        } else { p.fillRect(target,QColor(55,58,61)); requestTile(key,level,tx,ty,sourceTile); }
    }
    Q_EMIT tileProgress(QStringLiteral("加载可视图块"),completed,total);

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

        if(selectedCell_.x()>=0&&selectedCell_.y()>=0) {
            const ImageRegion selectedRegion=segmentationCellRegion(
                segmentation_,document_->metadata.width,document_->metadata.height,
                selectedCell_.y(),selectedCell_.x());
            if(selectedRegion.isValid()) {
                const QRectF selectedRect(baseX+selectedRegion.x*zoom_,baseY+selectedRegion.y*zoom_,
                                          selectedRegion.width*zoom_,selectedRegion.height*zoom_);
                p.fillRect(selectedRect,QColor(255,200,0,80));
                p.setPen(QPen(QColor(255,215,0,240),2));
                p.drawRect(selectedRect);
            }
        }
        p.restore();
    }
}

void LargeImageView::resizeEvent(QResizeEvent*) { update(); }
void LargeImageView::mousePressEvent(QMouseEvent* e) {
    if(e->button()!=Qt::LeftButton)return;
    leftPressed_=true;dragging_=false;pressPosition_=lastMouse_=e->pos();e->accept();
}
void LargeImageView::mouseMoveEvent(QMouseEvent* e) {
    if(!leftPressed_)return;
    if(!dragging_&&(e->pos()-pressPosition_).manhattanLength()<QApplication::startDragDistance())return;
    if(!dragging_){dragging_=true;setCursor(Qt::ClosedHandCursor);}
    offset_+=e->pos()-lastMouse_;lastMouse_=e->pos();update();e->accept();
}
void LargeImageView::mouseReleaseEvent(QMouseEvent* e) {
    if(e->button()!=Qt::LeftButton||!leftPressed_)return;
    leftPressed_=false;
    if(dragging_){dragging_=false;unsetCursor();}
    else selectCellAt(e->pos());
    e->accept();
}

void LargeImageView::selectCellAt(const QPoint& viewportPosition) {
    if(!document_||!showGrid_||zoom_<=0.0){clearSelectedCell();update();return;}
    const QPointF imagePosition=(QPointF(viewportPosition)-offset_)/zoom_;
    if(imagePosition.x()<0.0||imagePosition.y()<0.0||
       imagePosition.x()>=document_->metadata.width||imagePosition.y()>=document_->metadata.height){
        clearSelectedCell();update();return;
    }
    const int column=int(std::floor(imagePosition.x()/segmentation_.settings.cellWidth));
    const int row=int(std::floor(imagePosition.y()/segmentation_.settings.cellHeight));
    const ImageRegion region=segmentationCellRegion(
        segmentation_,document_->metadata.width,document_->metadata.height,row,column);
    if(!region.isValid()){clearSelectedCell();update();return;}
    selectedCell_={column,row};Q_EMIT segmentationCellSelected(row,column);update();
}

void LargeImageView::clearSelectedCell() {
    if(selectedCell_.x()<0&&selectedCell_.y()<0)return;
    selectedCell_={-1,-1};Q_EMIT segmentationCellSelected(-1,-1);
}
void LargeImageView::wheelEvent(QWheelEvent* e) {
    if(!document_)return; const QPointF pos=e->position(); const QPointF imagePos=(pos-offset_)/zoom_;
    zoom_=std::clamp(zoom_*std::pow(1.0015,e->angleDelta().y()),0.00001,64.0);
    offset_=pos-imagePos*zoom_; emitStatus(); update(); e->accept();
}

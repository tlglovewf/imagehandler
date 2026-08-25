#include "ui/LargeImageView.h"
#include "imaging/VipsSupport.h"

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
    document_ = document; cache_.clear(); pending_.clear(); showGrid_ = false;
    fitToWindow(); update();
}

void LargeImageView::setSegmentation(const SegmentationResult& segmentation) {
    segmentation_ = segmentation; showGrid_ = segmentation.rows > 0 && segmentation.columns > 0; update();
}

void LargeImageView::clearSegmentation() { showGrid_ = false; update(); }

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
    if (!document_ || pending_.contains(key)) return;
    pending_.insert(key);
    const auto doc = document_; const quint64 generation = doc->generation;
    const double scale = std::pow(2.0, -level);
    auto* watcher = new QFutureWatcher<QPair<QImage,QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<QImage,QString>>::finished, this, [this, watcher, key, generation] {
        const auto result = watcher->result(); watcher->deleteLater(); pending_.remove(key);
        if (document_ && document_->generation == generation && !result.first.isNull()) {
            auto* image = new QImage(result.first);
            cache_.insert(key, image, std::max(1, int(image->sizeInBytes()/1024)));
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
    qint64 total=0,ready=0;
    for(int ty=ty0;ty<ty1;++ty) for(int tx=tx0;tx<tx1;++tx) {
        ++total; const QString key=tileKey(level,tx,ty);
        const qint64 sourceX = qint64(tx) * sourceTile;
        const qint64 sourceY = qint64(ty) * sourceTile;
        const qint64 actualSourceWidth = std::min<qint64>(sourceTile, document_->metadata.width - sourceX);
        const qint64 actualSourceHeight = std::min<qint64>(sourceTile, document_->metadata.height - sourceY);
        if (actualSourceWidth <= 0 || actualSourceHeight <= 0) continue;
        const QRectF target(offset_.x()+tx*targetTile, offset_.y()+ty*targetTile,
                            actualSourceWidth*zoom_, actualSourceHeight*zoom_);
        if(QImage* image=cache_.object(key)) { p.drawImage(target,*image); ++ready; }
        else { p.fillRect(target,QColor(55,58,61)); requestTile(key,level,tx,ty,sourceTile); }
    }
    Q_EMIT tileProgress(QStringLiteral("加载可视图块"),ready,total);

    if(showGrid_) {
        p.save();
        const double sx=segmentation_.settings.cellWidth*zoom_, sy=segmentation_.settings.cellHeight*zoom_;
        const double usedW=segmentation_.columns*sx, usedH=segmentation_.rows*sy;
        const QRectF gridRect(offset_.x(), offset_.y(), usedW, usedH);
        const QRectF visibleGridRect = gridRect.intersected(QRectF(rect()));
        p.setClipRect(visibleImageRect);

        // Dense grids remain visible without obscuring the image. Only iterate lines
        // which can actually appear in the current viewport.
        const double minimumSpacing = std::min(sx, sy);
        const int alpha = minimumSpacing >= 16.0 ? 190 : minimumSpacing >= 8.0 ? 115 : 70;
        p.setPen(QPen(QColor(255,70,70,alpha),1));
        if(!visibleGridRect.isEmpty() && sx>=4) {
            const int first=std::max(0,int(std::ceil((visibleGridRect.left()-offset_.x())/sx)));
            const int last=std::min(segmentation_.columns,int(std::floor((visibleGridRect.right()-offset_.x())/sx)));
            for(int c=first;c<=last;++c) { const double x=offset_.x()+c*sx; p.drawLine(QPointF(x,gridRect.top()),QPointF(x,gridRect.bottom())); }
        }
        if(!visibleGridRect.isEmpty() && sy>=4) {
            const int first=std::max(0,int(std::ceil((visibleGridRect.top()-offset_.y())/sy)));
            const int last=std::min(segmentation_.rows,int(std::floor((visibleGridRect.bottom()-offset_.y())/sy)));
            for(int r=first;r<=last;++r) { const double y=offset_.y()+r*sy; p.drawLine(QPointF(gridRect.left(),y),QPointF(gridRect.right(),y)); }
        }

        p.setBrush(QColor(255,180,0,65)); p.setPen(Qt::NoPen);
        if(segmentation_.remainderWidth) p.drawRect(QRectF(offset_.x()+usedW,offset_.y(),segmentation_.remainderWidth*zoom_,document_->metadata.height*zoom_));
        if(segmentation_.remainderHeight) p.drawRect(QRectF(offset_.x(),offset_.y()+usedH,usedW,segmentation_.remainderHeight*zoom_));
        p.restore();
    }
}

void LargeImageView::resizeEvent(QResizeEvent*) { update(); }
void LargeImageView::mousePressEvent(QMouseEvent* e) { if(e->button()==Qt::LeftButton){dragging_=true;lastMouse_=e->pos();setCursor(Qt::ClosedHandCursor);} }
void LargeImageView::mouseMoveEvent(QMouseEvent* e) { if(dragging_){offset_+=e->pos()-lastMouse_;lastMouse_=e->pos();update();} }
void LargeImageView::mouseReleaseEvent(QMouseEvent* e) { if(e->button()==Qt::LeftButton){dragging_=false;unsetCursor();} }
void LargeImageView::wheelEvent(QWheelEvent* e) {
    if(!document_)return; const QPointF pos=e->position(); const QPointF imagePos=(pos-offset_)/zoom_;
    zoom_=std::clamp(zoom_*std::pow(1.0015,e->angleDelta().y()),0.00001,64.0);
    offset_=pos-imagePos*zoom_; emitStatus(); update(); e->accept();
}

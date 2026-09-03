#include "ui/JobPreviewWidget.h"
#include "imaging/JobLabelLod.h"
#include "imaging/JobPreviewMath.h"
#include "imaging/JobSpatialIndex.h"

#include <QCheckBox>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyleHints>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <vector>

#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QSet>
#include <QTimer>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QVulkanWindow>
#include <QVector4D>
#include <QWheelEvent>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr int kMaximumOrdinaryLabels = 4096;
constexpr size_t kMaximumLabelCandidates = 65536;

struct PointVertex { float x, y, z, id, r, g, b; };
struct LineVertex { float x, y, r, g, b; };
struct alignas(16) CameraUniform {
    float mvp[16];
    float selectedId = -1.0F;
    float hoveredId = -1.0F;
    float viewportSize[2]{1.0F,1.0F};
};

class JobVulkanWindow;

class JobVulkanRenderer final : public QVulkanWindowRenderer {
public:
    explicit JobVulkanRenderer(JobVulkanWindow* window) : window_(window) {}
    void initResources() override;
    void initSwapChainResources() override {}
    void releaseSwapChainResources() override {}
    void releaseResources() override;
    void startNextFrame() override;

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize capacity = 0;
        VkDeviceSize allocationSize = 0;
    };

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer);
    void destroyBuffer(Buffer* buffer);
    bool upload(Buffer* buffer, const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    VkShaderModule loadShader(const QString& resourcePath);
    VkPipeline createPipeline(VkShaderModule vertex, VkShaderModule fragment,
                              VkPrimitiveTopology topology, bool pointPipeline);
    void rebuildPointBuffer();

    JobVulkanWindow* window_ = nullptr;
    QVulkanDeviceFunctions* deviceFunctions_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipeline pointPipeline_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;
    Buffer pointBuffer_;
    std::vector<Buffer> uniformBuffers_;
    std::vector<Buffer> lineBuffers_;
    std::vector<VkDescriptorSet> descriptorSets_;
    bool initialized_ = false;
};

class JobVulkanWindow final : public QVulkanWindow {
public:
    JobVulkanWindow() {
        setFlags(PersistentResources);
        interactionTimer_.setInterval(16);
        QObject::connect(&interactionTimer_, &QTimer::timeout, this, [this] { tickInteraction(); });
    }

    QVulkanWindowRenderer* createRenderer() override {
        renderer_ = new JobVulkanRenderer(this);
        return renderer_;
    }

    void setDocument(const JobDocumentPtr& document) {
        document_ = document;
        selectedId_ = hoveredId_ = -1;
        idToIndex_.clear();
        origin_ = {};
        if (document_ && document_->bounds.valid) {
            origin_ = document_->bounds.center();
            idToIndex_.resize(document_->points.size(), -1);
            for (size_t index = 0; index < document_->points.size(); ++index) {
                const qint64 id = document_->points[index].placementId;
                if (id >= 0 && id < static_cast<qint64>(idToIndex_.size()))
                    idToIndex_[static_cast<size_t>(id)] = static_cast<qint64>(index);
            }
            camera_.setSceneBounds(document_->bounds.minimum.relativeTo(origin_),
                                   document_->bounds.maximum.relativeTo(origin_));
        } else {
            camera_.setSceneBounds({-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F});
        }
        animationActive_ = false;
        activeKeys_.clear();
        interactionTimer_.stop();
        pointBufferDirty_ = true;
        requestUpdate();
    }

    void setSelectedId(qint64 id, bool focus, bool preserveZoom) {
        if (id < 0 || id >= static_cast<qint64>(idToIndex_.size()) ||
            idToIndex_[static_cast<size_t>(id)] < 0) id = -1;
        selectedId_ = id;
        if (focus && id >= 0) focusSelected(preserveZoom);
        requestUpdate();
    }

    void setGuidesVisible(bool visible) { guidesVisible_ = visible; requestUpdate(); }

    void fitAll() {
        animateTo(camera_.fittedPose());
    }

    [[nodiscard]] const JobDocumentPtr& document() const { return document_; }
    [[nodiscard]] JobCoordinate origin() const { return origin_; }
    [[nodiscard]] qint64 selectedId() const { return selectedId_; }
    [[nodiscard]] qint64 hoveredId() const { return hoveredId_; }
    [[nodiscard]] bool takePointBufferDirty() { return std::exchange(pointBufferDirty_, false); }

    QMatrix4x4 viewProjection() const {
        const QSize imageSize = swapChainImageSize().isValid() ? swapChainImageSize() : size();
        const float aspect = imageSize.height() > 0 ? float(imageSize.width()) / imageSize.height() : 1.0F;
        QMatrix4x4 projection;
        projection.perspective(45.0F, aspect,
                               std::max(0.001F, camera_.pose().distance * 0.001F),
                               std::max(100.0F, camera_.pose().distance + camera_.sceneRadius() * 8.0F));
        return const_cast<JobVulkanWindow*>(this)->clipCorrectionMatrix() * projection * camera_.viewMatrix();
    }

    std::vector<LineVertex> buildLineVertices() const {
        std::vector<LineVertex> lines;
        if (!document_ || !document_->bounds.valid || width() <= 0 || height() <= 0) return lines;
        lines.reserve(128 * 2 + 4096 * 36);
        const QMatrix4x4 mvp = viewProjection();
        if (guidesVisible_) appendGuides(lines, mvp);
        appendLabels(lines, mvp);
        return lines;
    }

    std::function<void(qint64)> selectedCallback;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton &&
            event->button() != Qt::RightButton) return;
        requestActivate();
        cancelAnimation();
        pressedButton_ = event->button();
        pressPosition_ = lastPosition_ = event->position();
        dragging_ = false;
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (pressedButton_ != Qt::NoButton) {
            const QPointF delta = event->position() - lastPosition_;
            const QPointF pressDelta=event->position()-pressPosition_;
            const int dragDistance=QGuiApplication::styleHints()->startDragDistance();
            if (!dragging_ && QPointF::dotProduct(pressDelta,pressDelta)>=dragDistance*dragDistance)
                dragging_ = true;
            if (dragging_) {
                if (pressedButton_ == Qt::LeftButton) {
                    camera_.orbit(delta);
                } else if (pressedButton_ == Qt::MiddleButton) {
                    panOnGround(lastPosition_, event->position());
                } else
                    zoomAt(pressPosition_, std::exp(float(delta.y()) * 0.01F));
                requestUpdate();
            }
            lastPosition_ = event->position();
            event->accept();
            return;
        }
        if (!hoverTimer_.isValid() || hoverTimer_.elapsed() >= 45) {
            hoverTimer_.restart();
            const qint64 next = pick(event->position(), 8.0F, false);
            if (next != hoveredId_) { hoveredId_ = next; requestUpdate(); }
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != pressedButton_) return;
        if (!dragging_ && event->button() == Qt::LeftButton) {
            selectedId_ = pick(event->position(), 8.0F, true);
            if (selectedCallback) selectedCallback(selectedId_);
            requestUpdate();
        }
        pressedButton_ = Qt::NoButton;
        dragging_ = false;
        event->accept();
    }

    void wheelEvent(QWheelEvent* event) override {
        cancelAnimation();
        zoomAt(event->position(), std::exp(-float(event->angleDelta().y()) * 0.0012F));
        requestUpdate(); event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_F) { focusSelected(false); event->accept(); return; }
        if (event->key() == Qt::Key_Home) { fitAll(); event->accept(); return; }
        if (isMovementKey(event->key()) || event->key() == Qt::Key_Shift) {
            if (event->isAutoRepeat()) { event->accept(); return; }
            cancelAnimation();
            activeKeys_.insert(event->key());
            startInteractionTimer();
            event->accept();
            return;
        }
        QVulkanWindow::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        if (isMovementKey(event->key()) || event->key() == Qt::Key_Shift) {
            if (event->isAutoRepeat()) { event->accept(); return; }
            activeKeys_.remove(event->key());
            stopInteractionTimerIfIdle();
            event->accept();
            return;
        }
        QVulkanWindow::keyReleaseEvent(event);
    }

    void focusOutEvent(QFocusEvent* event) override {
        activeKeys_.clear();
        stopInteractionTimerIfIdle();
        QVulkanWindow::focusOutEvent(event);
    }

    bool event(QEvent* event) override {
        if (event->type() == QEvent::Leave && hoveredId_ != -1) {
            hoveredId_ = -1;
            requestUpdate();
        }
        return QVulkanWindow::event(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        selectedId_ = pick(event->position(), 8.0F, true);
        if (selectedCallback) selectedCallback(selectedId_);
        if (selectedId_ >= 0) focusSelected(false);
        event->accept();
    }

private:
    friend class JobVulkanRenderer;

    QVector3D localPosition(const JobPoint& point) const { return point.position.relativeTo(origin_); }

    bool project(const QVector3D& point, const QMatrix4x4& mvp, QPointF* screen,
                 QVector2D* ndc = nullptr, float* depth = nullptr) const {
        return JobPreviewMath::project(point, mvp, QSizeF(width(), height()), screen, ndc, depth);
    }

    qint64 pick(const QPointF& position, float radius, bool accurate) const {
        if (!document_) return -1;
        const QMatrix4x4 mvp = viewProjection();
        std::vector<quint32> candidates;
        if(document_->spatialIndex){QVector3D rayOrigin,rayDirection;if(JobPreviewMath::screenRay(position,QSizeF(width(),height()),mvp,&rayOrigin,&rayDirection)){const float angular=radius*2.0F*std::tan(22.5F*kPi/180.0F)/std::max(1,height());document_->spatialIndex->queryRay(rayOrigin,rayDirection,angular,origin_,&candidates,accurate?std::numeric_limits<size_t>::max():8192);}}
        if(!document_->spatialIndex){candidates.resize(document_->points.size());std::iota(candidates.begin(),candidates.end(),0U);}
        const float radiusSquared = radius * radius;
        float bestDepth = std::numeric_limits<float>::max();
        qint64 best = -1;
        const auto testPoint=[&](quint32 pointIndex){
            const JobPoint& point=document_->points[pointIndex];
            QPointF screen; float depth = 0.0F;
            if (!project(localPosition(point), mvp, &screen, nullptr, &depth)) return;
            const QPointF delta = screen - position;
            if (QPointF::dotProduct(delta, delta) <= radiusSquared && depth < bestDepth) {
                bestDepth = depth; best = point.placementId;
            }
        };
        for (quint32 pointIndex : candidates) testPoint(pointIndex);
        // A click must remain exact even if the conservative spatial-index cone loses a
        // point because of a very oblique camera or floating-point precision. Hovering
        // stays bounded, while the less frequent click may scan the full document.
        if(accurate&&best<0&&document_->spatialIndex){
            for(size_t pointIndex=0;pointIndex<document_->points.size();++pointIndex)
                testPoint(static_cast<quint32>(pointIndex));
        }
        return best;
    }

    bool groundIntersection(const QPointF& screen, QVector3D* intersection) const {
        QVector3D rayOrigin, rayDirection;
        return JobPreviewMath::screenRay(screen, QSizeF(width(), height()), viewProjection(),
                                         &rayOrigin, &rayDirection) &&
               JobPreviewMath::intersectHorizontalPlane(rayOrigin, rayDirection,
                                                         camera_.pose().pivot.z(), intersection);
    }

    void panOnGround(const QPointF& previous, const QPointF& current) {
        QVector3D before, after;
        if (groundIntersection(previous, &before) && groundIntersection(current, &after)) {
            QVector3D delta = before - after;
            delta.setZ(0.0F);
            camera_.translate(delta);
            return;
        }
        const QPointF pixels = current - previous;
        const float worldPerPixel = std::max(0.000001F,
            2.0F * camera_.pose().distance * std::tan(22.5F * kPi / 180.0F) /
                std::max(1, height()));
        camera_.translate(camera_.right() * float(-pixels.x()) * worldPerPixel +
                          camera_.groundForward() * float(pixels.y()) * worldPerPixel);
    }

    void zoomAt(const QPointF& screen, float factor) {
        QVector3D before, after;
        const bool anchored = groundIntersection(screen, &before);
        camera_.zoomBy(factor);
        if (anchored && groundIntersection(screen, &after)) {
            QVector3D correction = before - after;
            correction.setZ(0.0F);
            camera_.translate(correction);
        }
    }

    void focusSelected(bool preserveZoom) {
        if (!document_ || selectedId_ < 0 || selectedId_ >= static_cast<qint64>(idToIndex_.size())) return;
        const qint64 index = idToIndex_[static_cast<size_t>(selectedId_)];
        if (index < 0) return;
        animateTo(camera_.focusPose(localPosition(document_->points[static_cast<size_t>(index)]),
                                    preserveZoom));
    }

    static bool isMovementKey(int key) {
        return key == Qt::Key_W || key == Qt::Key_A || key == Qt::Key_S ||
               key == Qt::Key_D || key == Qt::Key_Q || key == Qt::Key_E;
    }

    void startInteractionTimer() {
        if (!interactionTimer_.isActive()) {
            frameClock_.restart();
            interactionTimer_.start();
        }
    }

    void stopInteractionTimerIfIdle() {
        if (activeKeys_.isEmpty() && !animationActive_) interactionTimer_.stop();
    }

    void cancelAnimation() {
        animationActive_ = false;
        stopInteractionTimerIfIdle();
    }

    void animateTo(const JobCameraPose& target) {
        animationStart_ = camera_.pose();
        animationTarget_ = target;
        animationClock_.restart();
        animationActive_ = true;
        startInteractionTimer();
    }

    void tickInteraction() {
        const float deltaSeconds = std::clamp(frameClock_.restart() / 1000.0F, 0.0F, 0.05F);
        bool changed = false;
        if (animationActive_) {
            const float linear = std::clamp(animationClock_.elapsed() / 220.0F, 0.0F, 1.0F);
            const float eased = 1.0F - std::pow(1.0F - linear, 3.0F);
            JobCameraPose pose;
            pose.pivot = animationStart_.pivot * (1.0F - eased) + animationTarget_.pivot * eased;
            pose.distance = animationStart_.distance * (1.0F - eased) + animationTarget_.distance * eased;
            const float azimuthDelta = std::remainder(animationTarget_.azimuth - animationStart_.azimuth, 360.0F);
            pose.azimuth = animationStart_.azimuth + azimuthDelta * eased;
            pose.elevation = animationStart_.elevation * (1.0F - eased) + animationTarget_.elevation * eased;
            camera_.setPose(pose);
            changed = true;
            if (linear >= 1.0F) animationActive_ = false;
        }
        if (!activeKeys_.isEmpty() && deltaSeconds > 0.0F) {
            float speed = std::max(camera_.sceneRadius() * 0.05F,
                                   camera_.pose().distance * 0.6F);
            if (activeKeys_.contains(Qt::Key_Shift)) speed *= 4.0F;
            const float amount = speed * deltaSeconds;
            QVector3D movement;
            if (activeKeys_.contains(Qt::Key_W)) movement += camera_.groundForward();
            if (activeKeys_.contains(Qt::Key_S)) movement -= camera_.groundForward();
            if (activeKeys_.contains(Qt::Key_A)) movement -= camera_.right();
            if (activeKeys_.contains(Qt::Key_D)) movement += camera_.right();
            if (activeKeys_.contains(Qt::Key_Q)) movement.setZ(movement.z() - 1.0F);
            if (activeKeys_.contains(Qt::Key_E)) movement.setZ(movement.z() + 1.0F);
            if (!movement.isNull()) camera_.translate(movement.normalized() * amount);
            changed = true;
        }
        if (changed) requestUpdate();
        stopInteractionTimerIfIdle();
    }

    static void appendLine(std::vector<LineVertex>& lines, const QVector2D& a, const QVector2D& b,
                           const QVector3D& color) {
        lines.push_back({a.x(), a.y(), color.x(), color.y(), color.z()});
        lines.push_back({b.x(), b.y(), color.x(), color.y(), color.z()});
    }

    void appendWorldLine(std::vector<LineVertex>& lines, const QVector3D& a, const QVector3D& b,
                         const QVector3D& color, const QMatrix4x4& mvp) const {
        QVector2D an, bn;
        if (!project(a, mvp, nullptr, &an) || !project(b, mvp, nullptr, &bn)) return;
        appendLine(lines, an, bn, color);
    }

    void appendGuides(std::vector<LineVertex>& lines, const QMatrix4x4& mvp) const {
        const JobGuideGeometry guides = JobPreviewMath::guideGeometry(*document_, origin_);
        if (!guides.valid) return;
        const float extent = std::max({1.0F, static_cast<float>(guides.xDimension),
                                      static_cast<float>(guides.yDimension)});
        const float exponent = std::floor(std::log10(extent / 10.0F));
        const float step = std::max(0.000001F, std::pow(10.0F, exponent));
        const QVector3D gridColor(0.20F, 0.23F, 0.27F);
        const float firstX = guides.pointZero.x() +
            std::floor((guides.minimum.x() - guides.pointZero.x()) / step) * step;
        const float firstY = guides.pointZero.y() +
            std::floor((guides.minimum.y() - guides.pointZero.y()) / step) * step;
        for (float value = firstX; value <= guides.maximum.x() + step * 0.5F; value += step) {
            appendWorldLine(lines, {value, guides.minimum.y(), guides.pointZero.z()},
                            {value, guides.maximum.y(), guides.pointZero.z()}, gridColor, mvp);
        }
        for (float value = firstY; value <= guides.maximum.y() + step * 0.5F; value += step) {
            appendWorldLine(lines, {guides.minimum.x(), value, guides.pointZero.z()},
                            {guides.maximum.x(), value, guides.pointZero.z()}, gridColor, mvp);
        }
        const QVector3D xColor(1.0F, 0.25F, 0.25F);
        const QVector3D yColor(0.25F, 1.0F, 0.35F);
        appendWorldLine(lines, guides.xStart, guides.xEnd, xColor, mvp);
        appendWorldLine(lines, guides.yStart, guides.yEnd, yColor, mvp);
        const float zLength = std::max(step * 2.0F, extent * 0.12F);
        appendWorldLine(lines, guides.pointZero,
                        guides.pointZero + QVector3D(0, 0, zLength),
                        {0.25F, 0.55F, 1.0F}, mvp);

        QPointF xStartScreen, xEndScreen, yStartScreen, yEndScreen;
        if (project(guides.xStart, mvp, &xStartScreen) && project(guides.xEnd, mvp, &xEndScreen)) {
            const QPointF midpoint = (xStartScreen + xEndScreen) * 0.5 + QPointF(5.0, 8.0);
            appendOutlinedText(lines, midpoint,
                               QStringLiteral("X %1").arg(formatDimension(guides.xDimension)), xColor);
        }
        if (project(guides.yStart, mvp, &yStartScreen) && project(guides.yEnd, mvp, &yEndScreen)) {
            const QPointF midpoint = (yStartScreen + yEndScreen) * 0.5 + QPointF(8.0, -8.0);
            appendOutlinedText(lines, midpoint,
                               QStringLiteral("Y %1").arg(formatDimension(guides.yDimension)), yColor);
        }
    }

    void appendGlyph(std::vector<LineVertex>& lines, float x, float y, QChar glyph,
                     const QVector3D& color) const {
        static constexpr unsigned char masks[10] = {
            0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
        };
        const float w = 6.0F, h = 10.0F;
        const QPointF points[6] = {{x, y}, {x+w, y}, {x, y+h*0.5F}, {x+w, y+h*0.5F},
                                   {x, y+h}, {x+w, y+h}};
        const int pairs[7][2] = {{0,1},{1,3},{3,5},{4,5},{2,4},{0,2},{2,3}};
        unsigned char mask = 0;
        if (glyph.isDigit()) mask = masks[glyph.digitValue()];
        else if (glyph == QLatin1Char('-')) mask = 0x40;
        if (mask) {
            for (int segment = 0; segment < 7; ++segment) {
                if (!(mask & (1U << segment))) continue;
                const QPointF a = points[pairs[segment][0]], b = points[pairs[segment][1]];
                appendLine(lines, pixelToNdc(a), pixelToNdc(b), color);
            }
        } else if (glyph == QLatin1Char('~')) {
            appendLine(lines, pixelToNdc({x, y+6}), pixelToNdc({x+2, y+4}), color);
            appendLine(lines, pixelToNdc({x+2, y+4}), pixelToNdc({x+4, y+6}), color);
            appendLine(lines, pixelToNdc({x+4, y+6}), pixelToNdc({x+6, y+4}), color);
        } else if (glyph == QLatin1Char('.')) {
            appendLine(lines, pixelToNdc({x+2, y+h}), pixelToNdc({x+3, y+h}), color);
        } else if (glyph == QLatin1Char('X')) {
            appendLine(lines, pixelToNdc({x, y}), pixelToNdc({x+w, y+h}), color);
            appendLine(lines, pixelToNdc({x+w, y}), pixelToNdc({x, y+h}), color);
        } else if (glyph == QLatin1Char('Y')) {
            appendLine(lines, pixelToNdc({x, y}), pixelToNdc({x+w*0.5F, y+h*0.5F}), color);
            appendLine(lines, pixelToNdc({x+w, y}), pixelToNdc({x+w*0.5F, y+h*0.5F}), color);
            appendLine(lines, pixelToNdc({x+w*0.5F, y+h*0.5F}), pixelToNdc({x+w*0.5F, y+h}), color);
        }
    }

    QVector2D pixelToNdc(const QPointF& point) const {
        return JobPreviewMath::screenToVulkanNdc(point, QSizeF(width(), height()));
    }

    void appendText(std::vector<LineVertex>& lines, const QPointF& anchor, const QString& text,
                    const QVector3D& color) const {
        float x = float(anchor.x()) + 7.0F;
        const float y = float(anchor.y()) - 6.0F;
        for (QChar glyph : text) { appendGlyph(lines, x, y, glyph, color); x += 8.0F; }
    }

    void appendOutlinedText(std::vector<LineVertex>& lines, const QPointF& anchor,
                            const QString& text, const QVector3D& color) const {
        const QVector3D outline(0.96F, 0.96F, 0.96F);
        static constexpr QPointF offsets[] = {
            {-1.5, 0.0}, {1.5, 0.0}, {0.0, -1.5}, {0.0, 1.5},
            {-1.0, -1.0}, {1.0, -1.0}, {-1.0, 1.0}, {1.0, 1.0}
        };
        for (const QPointF& offset : offsets) appendText(lines, anchor + offset, text, outline);
        appendText(lines, anchor, text, color);
    }

    static QString formatDimension(double value) {
        QString text = QString::number(value, 'f', 3);
        while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0'))) text.chop(1);
        if (text.endsWith(QLatin1Char('.'))) text.chop(1);
        return text;
    }

    void appendLabels(std::vector<LineVertex>& lines, const QMatrix4x4& mvp) const {
        if (document_->points.empty()) return;
        std::vector<quint32> visible;
        if(document_->spatialIndex)document_->spatialIndex->queryVisible(
            mvp,origin_,&visible,kMaximumLabelCandidates);
        if(!document_->spatialIndex){visible.resize(document_->points.size());std::iota(visible.begin(),visible.end(),0U);}

        struct ProjectedPoint { quint32 pointIndex; QPointF screen; };
        std::vector<ProjectedPoint> projected;
        std::vector<QPointF> projectedScreens;
        projected.reserve(visible.size());
        projectedScreens.reserve(visible.size());
        for (quint32 pointIndex : visible) {
            QPointF screen;
            if (!project(localPosition(document_->points[pointIndex]),mvp,&screen) ||
                screen.x()<0.0 || screen.y()<0.0 || screen.x()>=width() || screen.y()>=height())
                continue;
            projected.push_back({pointIndex,screen});
            projectedScreens.push_back(screen);
        }
        const double spacing = JobPreviewMath::projectedPointSpacing(
            projectedScreens,projectedScreens.size());
        QSet<quint64> occupied;
        int ordinary = 0;
        const auto addOrdinary = [&](const QPointF& screen, const QString& text) {
            if (screen.x() < 0 || screen.y() < 0 || screen.x() >= width() || screen.y() >= height() ||
                ordinary >= kMaximumOrdinaryLabels) return;
            const quint32 gx = static_cast<quint32>(screen.x() / 80.0);
            const quint32 gy = static_cast<quint32>(screen.y() / 22.0);
            const quint64 key = (quint64(gx) << 32) | gy;
            if (occupied.contains(key)) return;
            occupied.insert(key); ++ordinary;
            appendText(lines, screen, text, {0.72F, 0.82F, 0.90F});
        };
        const JobLabelLodDecision lod = JobLabelLod::decide(spacing, static_cast<qint64>(document_->points.size()));
        if (lod.level == JobLabelLodLevel::Individual) {
            for (const ProjectedPoint& point : projected) {
                const qint64 id=document_->points[point.pointIndex].placementId;
                if (id == selectedId_ || id == hoveredId_) continue;
                addOrdinary(point.screen, QString::number(id));
            }
        } else if (lod.level == JobLabelLodLevel::Range) {
            const qint64 group = lod.rangeSize;
            struct RangeAnchor { QPointF screenSum; qint64 visibleCount=0; };
            std::map<qint64,RangeAnchor> anchors;
            for(const ProjectedPoint& point:projected){
                const qint64 id=document_->points[point.pointIndex].placementId;
                if(id<0)continue;
                auto& anchor=anchors[(id/group)*group];
                anchor.screenSum+=point.screen;++anchor.visibleCount;
            }
            for(const auto& [firstId,anchor]:anchors){
                if(anchor.visibleCount<=0)continue;
                const qint64 lastId=std::min<qint64>(firstId+group-1,idToIndex_.size()-1);
                addOrdinary(anchor.screenSum/anchor.visibleCount,
                            QStringLiteral("%1~%2").arg(firstId).arg(lastId));
            }
        }
        const auto addPriority = [&](qint64 id, const QVector3D& color) {
            if (id < 0 || id >= static_cast<qint64>(idToIndex_.size())) return;
            const qint64 index = idToIndex_[static_cast<size_t>(id)];
            if (index < 0) return;
            QPointF screen;
            if (project(localPosition(document_->points[static_cast<size_t>(index)]), mvp, &screen))
                appendText(lines, screen, QString::number(id), color);
        };
        addPriority(hoveredId_, {1.0F, 0.50F, 0.15F});
        addPriority(selectedId_, {1.0F, 0.85F, 0.0F});
    }

    JobVulkanRenderer* renderer_ = nullptr;
    JobDocumentPtr document_;
    JobCoordinate origin_;
    std::vector<qint64> idToIndex_;
    JobTerrainCamera camera_;
    QTimer interactionTimer_;
    QElapsedTimer frameClock_;
    QElapsedTimer animationClock_;
    QSet<int> activeKeys_;
    JobCameraPose animationStart_;
    JobCameraPose animationTarget_;
    bool animationActive_ = false;
    qint64 selectedId_ = -1;
    qint64 hoveredId_ = -1;
    bool guidesVisible_ = true;
    bool pointBufferDirty_ = true;
    Qt::MouseButton pressedButton_ = Qt::NoButton;
    QPointF pressPosition_;
    QPointF lastPosition_;
    bool dragging_ = false;
    mutable QElapsedTimer hoverTimer_;
};

bool JobVulkanRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer) {
    if (!buffer || size == 0) return false;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (deviceFunctions_->vkCreateBuffer(device_, &info, nullptr, &buffer->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    deviceFunctions_->vkGetBufferMemoryRequirements(device_, buffer->buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = window_->hostVisibleMemoryIndex();
    if (deviceFunctions_->vkAllocateMemory(device_, &allocation, nullptr, &buffer->memory) != VK_SUCCESS) {
        deviceFunctions_->vkDestroyBuffer(device_, buffer->buffer, nullptr); buffer->buffer = VK_NULL_HANDLE; return false;
    }
    if (deviceFunctions_->vkBindBufferMemory(device_, buffer->buffer, buffer->memory, 0) != VK_SUCCESS) {
        destroyBuffer(buffer); return false;
    }
    buffer->capacity = size;
    buffer->allocationSize = requirements.size;
    return true;
}

void JobVulkanRenderer::destroyBuffer(Buffer* buffer) {
    if (!buffer || !deviceFunctions_) return;
    if (buffer->buffer) deviceFunctions_->vkDestroyBuffer(device_, buffer->buffer, nullptr);
    if (buffer->memory) deviceFunctions_->vkFreeMemory(device_, buffer->memory, nullptr);
    *buffer = {};
}

bool JobVulkanRenderer::upload(Buffer* buffer, const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    if (!buffer || !data || !size) return false;
    if (buffer->capacity < size) {
        destroyBuffer(buffer);
        if (!createBuffer(std::max<VkDeviceSize>(size, size * 3 / 2), usage, buffer)) return false;
    }
    void* mapped = nullptr;
    if (deviceFunctions_->vkMapMemory(device_, buffer->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
    std::memcpy(mapped, data, static_cast<size_t>(size));
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = buffer->memory; range.offset = 0; range.size = VK_WHOLE_SIZE;
    deviceFunctions_->vkFlushMappedMemoryRanges(device_, 1, &range);
    deviceFunctions_->vkUnmapMemory(device_, buffer->memory);
    return true;
}

VkShaderModule JobVulkanRenderer::loadShader(const QString& resourcePath) {
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) return VK_NULL_HANDLE;
    const QByteArray code = file.readAll();
    if (code.isEmpty() || code.size() % 4) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = static_cast<size_t>(code.size());
    info.pCode = reinterpret_cast<const uint32_t*>(code.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    return deviceFunctions_->vkCreateShaderModule(device_, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

VkPipeline JobVulkanRenderer::createPipeline(VkShaderModule vertex, VkShaderModule fragment,
                                             VkPrimitiveTopology topology, bool pointPipeline) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment; stages[1].pName = "main";
    VkVertexInputBindingDescription binding{};
    binding.binding = 0; binding.inputRate = pointPipeline ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    binding.stride = pointPipeline ? sizeof(PointVertex) : sizeof(LineVertex);
    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0] = {0, 0, pointPipeline ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_R32G32_SFLOAT, 0};
    attributes[1] = {1, 0, pointPipeline ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32B32_SFLOAT,
                     pointPipeline ? 12U : 8U};
    if (pointPipeline) attributes[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 16U};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1; vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = pointPipeline ? 3U : 2U;
    vertexInput.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1; viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = window_->sampleCountFlagBits();
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = pointPipeline; depth.depthWriteEnable = pointPipeline;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xF;
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1; blend.pAttachments = &attachment;
    const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly; info.pViewportState = &viewport;
    info.pRasterizationState = &raster; info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth; info.pColorBlendState = &blend; info.pDynamicState = &dynamic;
    info.layout = pipelineLayout_; info.renderPass = window_->defaultRenderPass();
    VkPipeline pipeline = VK_NULL_HANDLE;
    return deviceFunctions_->vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) == VK_SUCCESS
               ? pipeline : VK_NULL_HANDLE;
}

void JobVulkanRenderer::initResources() {
    device_ = window_->device();
    deviceFunctions_ = window_->vulkanInstance()->deviceFunctions(device_);
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0; binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1; binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1; layoutInfo.pBindings = &binding;
    if (deviceFunctions_->vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorLayout_) != VK_SUCCESS) return;
    VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineInfo.setLayoutCount = 1; pipelineInfo.pSetLayouts = &descriptorLayout_;
    if (deviceFunctions_->vkCreatePipelineLayout(device_, &pipelineInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) return;
    const int frames = window_->concurrentFrameCount();
    uniformBuffers_.resize(frames); lineBuffers_.resize(frames); descriptorSets_.resize(frames);
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<uint32_t>(frames)};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = frames; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
    if (deviceFunctions_->vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) return;
    std::vector<VkDescriptorSetLayout> layouts(frames, descriptorLayout_);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool_; allocate.descriptorSetCount = frames; allocate.pSetLayouts = layouts.data();
    if (deviceFunctions_->vkAllocateDescriptorSets(device_, &allocate, descriptorSets_.data()) != VK_SUCCESS) return;
    for (int frame = 0; frame < frames; ++frame) {
        if (!createBuffer(sizeof(CameraUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &uniformBuffers_[frame])) return;
        VkDescriptorBufferInfo bufferInfo{uniformBuffers_[frame].buffer, 0, sizeof(CameraUniform)};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSets_[frame]; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo = &bufferInfo;
        deviceFunctions_->vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    VkShaderModule pointVertex = loadShader(QStringLiteral(":/job_shaders/job_points.vert.spv"));
    VkShaderModule pointFragment = loadShader(QStringLiteral(":/job_shaders/job_points.frag.spv"));
    VkShaderModule lineVertex = loadShader(QStringLiteral(":/job_shaders/job_lines.vert.spv"));
    VkShaderModule lineFragment = loadShader(QStringLiteral(":/job_shaders/job_lines.frag.spv"));
    if (pointVertex && pointFragment) pointPipeline_ = createPipeline(pointVertex, pointFragment, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true);
    if (lineVertex && lineFragment) linePipeline_ = createPipeline(lineVertex, lineFragment, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false);
    for (VkShaderModule module : {pointVertex, pointFragment, lineVertex, lineFragment})
        if (module) deviceFunctions_->vkDestroyShaderModule(device_, module, nullptr);
    initialized_ = pointPipeline_ && linePipeline_;
    rebuildPointBuffer();
}

void JobVulkanRenderer::rebuildPointBuffer() {
    deviceFunctions_->vkDeviceWaitIdle(device_);
    destroyBuffer(&pointBuffer_);
    const auto& document = window_->document();
    if (!document || document->points.empty()) return;
    std::vector<PointVertex> vertices;
    vertices.reserve(document->points.size());
    const JobCoordinate origin = window_->origin();
    for (const JobPoint& point : document->points) {
        const QVector3D local = point.position.relativeTo(origin);
        const QColor color = point.groupColor.isValid() ? point.groupColor
                                                        : QColor(26, 191, 255);
        vertices.push_back({local.x(), local.y(), local.z(),
                            static_cast<float>(point.placementId), color.redF(),
                            color.greenF(), color.blueF()});
    }
    upload(&pointBuffer_, vertices.data(), vertices.size() * sizeof(PointVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

void JobVulkanRenderer::startNextFrame() {
    if (!initialized_) { window_->frameReady(); return; }
    if (window_->takePointBufferDirty()) rebuildPointBuffer();
    const int frame = window_->currentFrame();
    CameraUniform uniform;
    const QMatrix4x4 matrix = window_->viewProjection();
    std::memcpy(uniform.mvp, matrix.constData(), sizeof(uniform.mvp));
    uniform.selectedId = static_cast<float>(window_->selectedId());
    uniform.hoveredId = static_cast<float>(window_->hoveredId());
    uniform.viewportSize[0]=static_cast<float>(std::max(1,window_->swapChainImageSize().width()));
    uniform.viewportSize[1]=static_cast<float>(std::max(1,window_->swapChainImageSize().height()));
    upload(&uniformBuffers_[frame], &uniform, sizeof(uniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const std::vector<LineVertex> lines = window_->buildLineVertices();
    if (!lines.empty()) upload(&lineBuffers_[frame], lines.data(), lines.size() * sizeof(LineVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    VkClearValue clearValues[2]{};
    clearValues[0].color = {{0.045F, 0.055F, 0.07F, 1.0F}};
    clearValues[1].depthStencil = {1.0F, 0};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = window_->defaultRenderPass(); begin.framebuffer = window_->currentFramebuffer();
    begin.renderArea.extent = {static_cast<uint32_t>(window_->swapChainImageSize().width()),
                               static_cast<uint32_t>(window_->swapChainImageSize().height())};
    begin.clearValueCount = 2; begin.pClearValues = clearValues;
    VkCommandBuffer command = window_->currentCommandBuffer();
    deviceFunctions_->vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, float(begin.renderArea.extent.width), float(begin.renderArea.extent.height), 0, 1};
    VkRect2D scissor{{0,0}, begin.renderArea.extent};
    deviceFunctions_->vkCmdSetViewport(command, 0, 1, &viewport);
    deviceFunctions_->vkCmdSetScissor(command, 0, 1, &scissor);
    const auto& document = window_->document();
    if (document && pointBuffer_.buffer) {
        const VkDeviceSize offset = 0;
        deviceFunctions_->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pointPipeline_);
        deviceFunctions_->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                   pipelineLayout_, 0, 1, &descriptorSets_[frame], 0, nullptr);
        deviceFunctions_->vkCmdBindVertexBuffers(command, 0, 1, &pointBuffer_.buffer, &offset);
        deviceFunctions_->vkCmdDraw(command, 6, static_cast<uint32_t>(document->points.size()), 0, 0);
    }
    if (!lines.empty() && lineBuffers_[frame].buffer) {
        const VkDeviceSize offset = 0;
        deviceFunctions_->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);
        deviceFunctions_->vkCmdBindVertexBuffers(command, 0, 1, &lineBuffers_[frame].buffer, &offset);
        deviceFunctions_->vkCmdDraw(command, static_cast<uint32_t>(lines.size()), 1, 0, 0);
    }
    deviceFunctions_->vkCmdEndRenderPass(command);
    window_->frameReady();
}

void JobVulkanRenderer::releaseResources() {
    if (!deviceFunctions_) return;
    for (auto& buffer : lineBuffers_) destroyBuffer(&buffer);
    for (auto& buffer : uniformBuffers_) destroyBuffer(&buffer);
    destroyBuffer(&pointBuffer_);
    if (pointPipeline_) deviceFunctions_->vkDestroyPipeline(device_, pointPipeline_, nullptr);
    if (linePipeline_) deviceFunctions_->vkDestroyPipeline(device_, linePipeline_, nullptr);
    if (pipelineLayout_) deviceFunctions_->vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorPool_) deviceFunctions_->vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (descriptorLayout_) deviceFunctions_->vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    pointPipeline_ = linePipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE; descriptorPool_ = VK_NULL_HANDLE; descriptorLayout_ = VK_NULL_HANDLE;
    initialized_ = false; deviceFunctions_ = nullptr; device_ = VK_NULL_HANDLE;
}

} // namespace
#endif

class JobPreviewWidget::Backend {
public:
    explicit Backend(JobPreviewWidget* owner, QWidget* host) : owner_(owner) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        instance_ = std::make_unique<QVulkanInstance>();
        if (!instance_->create()) {
            error_ = QStringLiteral("Vulkan 初始化失败（错误码 %1）。请更新显卡驱动并确认 Vulkan Runtime 可用。")
                         .arg(instance_->errorCode());
            return;
        }
        window_ = new JobVulkanWindow;
        window_->setObjectName(QStringLiteral("jobVulkanWindow"));
        window_->setVulkanInstance(instance_.get());
        if (window_->availablePhysicalDevices().isEmpty()) {
            delete window_; window_ = nullptr;
            error_ = QStringLiteral("未找到可用于 Job 预览的 Vulkan 图形设备。请更新显卡驱动或启用 GPU 直通。");
            return;
        }
        window_->selectedCallback = [this](qint64 id) {
            owner_->selectedPlaceId_=id;
            Q_EMIT owner_->placeSelected(id);
        };
        container_ = QWidget::createWindowContainer(window_, host);
        container_->setFocusPolicy(Qt::StrongFocus);
#else
        Q_UNUSED(host);
        error_ = QStringLiteral("此构建未启用 Vulkan Job 预览。请安装 Vulkan SDK 和 Qt ShaderTools 后重新配置。");
#endif
    }

    ~Backend() {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        delete container_;
        container_ = nullptr;
        window_ = nullptr;
#endif
    }

    [[nodiscard]] bool available() const {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        return window_ != nullptr;
#else
        return false;
#endif
    }
    [[nodiscard]] QWidget* widget() const {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        return container_;
#else
        return nullptr;
#endif
    }
    [[nodiscard]] QString error() const { return error_; }
    void setDocument(const JobDocumentPtr& document) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if (window_) window_->setDocument(document);
#else
        Q_UNUSED(document);
#endif
    }
    void setSelected(qint64 id, bool focus, bool preserveZoom) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if (window_) window_->setSelectedId(id, focus, preserveZoom);
#else
        Q_UNUSED(id); Q_UNUSED(focus); Q_UNUSED(preserveZoom);
#endif
    }
    void fitAll() {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if (window_) window_->fitAll();
#endif
    }
    void setGuides(bool visible) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if (window_) window_->setGuidesVisible(visible);
#else
        Q_UNUSED(visible);
#endif
    }
    QImage grabFrame() {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        return window_ ? window_->grab() : QImage();
#else
        return {};
#endif
    }

private:
    JobPreviewWidget* owner_ = nullptr;
    QString error_;
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
    std::unique_ptr<QVulkanInstance> instance_;
    JobVulkanWindow* window_ = nullptr;
    QWidget* container_ = nullptr;
#endif
};

JobPreviewWidget::JobPreviewWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("jobPreviewWidget"));
    auto* root = new QVBoxLayout(this); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(4);
    auto* tools = new QHBoxLayout;
    fitButton_ = new QPushButton(QStringLiteral("适配全部")); fitButton_->setObjectName(QStringLiteral("jobFitAllButton"));
    guidesCheck_ = new QCheckBox(QStringLiteral("坐标轴与网格")); guidesCheck_->setObjectName(QStringLiteral("jobGuidesCheck")); guidesCheck_->setChecked(true);
    statusLabel_ = new QLabel(QStringLiteral("未加载 Job")); statusLabel_->setObjectName(QStringLiteral("jobPreviewStatus"));
    tools->addWidget(fitButton_); tools->addWidget(guidesCheck_); tools->addStretch(); tools->addWidget(statusLabel_);
    root->addLayout(tools);
    backend_ = new Backend(this, this);
    if (backend_->available()) root->addWidget(backend_->widget(), 1);
    else {
        auto* error = new QLabel(backend_->error());
        error->setObjectName(QStringLiteral("jobVulkanError")); error->setAlignment(Qt::AlignCenter);
        error->setWordWrap(true); error->setStyleSheet(QStringLiteral("QLabel { color: #ff8a80; background: #25282b; padding: 24px; }"));
        root->addWidget(error, 1);
    }
    connect(fitButton_, &QPushButton::clicked, this, &JobPreviewWidget::fitAll);
    connect(guidesCheck_, &QCheckBox::toggled, this, [this](bool visible) { backend_->setGuides(visible); });
    fitButton_->setEnabled(backend_->available()); guidesCheck_->setEnabled(backend_->available());
}

JobPreviewWidget::~JobPreviewWidget() { delete backend_; }

void JobPreviewWidget::setDocument(const JobDocumentPtr& document) {
    document_ = document; selectedPlaceId_ = -1;
    backend_->setDocument(document_);
    if (!document_) statusLabel_->setText(QStringLiteral("未加载 Job"));
    else statusLabel_->setText(QStringLiteral("%1 个点 | %2")
                                   .arg(document_->points.size())
                                   .arg(document_->source == JobDocumentSource::ImportedFile
                                            ? QStringLiteral("手动导入（不联动）")
                                            : QStringLiteral("分割联动")));
}

bool JobPreviewWidget::isVulkanAvailable() const noexcept { return backend_ && backend_->available(); }

void JobPreviewWidget::setSelectedPlaceId(qint64 placeId, bool focus, bool preserveZoom) {
    selectedPlaceId_ = placeId;
    backend_->setSelected(placeId, focus, preserveZoom);
}

void JobPreviewWidget::clearSelection() { setSelectedPlaceId(-1, false); }
void JobPreviewWidget::fitAll() { if (backend_) backend_->fitAll(); }
QImage JobPreviewWidget::grabFrame() { return backend_ ? backend_->grabFrame() : QImage(); }

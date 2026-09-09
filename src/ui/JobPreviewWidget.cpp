#include "ui/JobPreviewWidget.h"
#include "domain/MaskSliceModels.h"
#include "imaging/JobLabelLod.h"
#include "imaging/JobPreviewMath.h"
#include "imaging/JobSpatialIndex.h"
#include "imaging/JobStackLod.h"
#include "imaging/MaskStackMeshBuilder.h"

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
#include <QFutureWatcher>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QSet>
#include <QTimer>
#include <QThreadPool>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QVulkanWindow>
#include <QVector4D>
#include <QWheelEvent>
#include <cstring>
#include <set>
#include <tuple>
#include <QtConcurrent>

namespace {

enum class JobStackDisplayMode { Solid, Exploded };

constexpr float kPi = 3.14159265358979323846F;
constexpr int kMaximumOrdinaryLabels = 4096;
constexpr size_t kMaximumLabelCandidates = 65536;

struct PointVertex { float x, y, z, id, r, g, b; };
struct LineVertex { float x, y, r, g, b; };
struct StackInstance { float x, y, z, id, layerCount, scaleX, scaleY, scaleZ; };
struct alignas(16) CameraUniform {
    float mvp[16];
    float selectedId = -1.0F;
    float hoveredId = -1.0F;
    float viewportSize[2]{1.0F,1.0F};
    float stackExplode = 1.0F;
    float padding[3]{};
};
static_assert(sizeof(MaskStackVertex) == 32);
static_assert(sizeof(StackInstance) == 32);
static_assert(sizeof(CameraUniform) == 96);

struct LodCacheKey {
    const JobModelStack* stack = nullptr;
    JobStackDisplayMode mode = JobStackDisplayMode::Solid;
    int level = 0;
    friend bool operator<(const LodCacheKey& left, const LodCacheKey& right) {
        return std::tie(left.stack, left.mode, left.level) <
               std::tie(right.stack, right.mode, right.level);
    }
};

struct LodBuildResult {
    LodCacheKey key;
    std::shared_ptr<const MaskStackGeometry> geometry;
    quint64 generation = 0;
};

struct GeometrySelection {
    std::shared_ptr<const MaskStackGeometry> geometry;
    int level = -1;
};

struct LodCacheEntry {
    std::shared_ptr<const MaskStackGeometry> geometry;
    std::size_t byteSize = 0;
    quint64 lastUsed = 0;
};

std::shared_ptr<const MaskStackGeometry> buildLodGeometry(
    const JobModelStackPtr& stack, int level, JobStackDisplayMode mode,
    const CancellationTokenPtr& token) {
    if (!stack || level < 0 || level >= static_cast<int>(stack->lodLevels.size())) return {};
    const JobStackHeightLevel& source = stack->lodLevels[static_cast<std::size_t>(level)];
    MaskStackPreviewData preview;
    preview.gridSize = source.gridSize;
    preview.requestedOutputSize = stack->sourceSize;
    preview.physicalSize = stack->physicalSize;
    preview.columnLayers = QVector<quint16>(source.columnLayers.cbegin(),
                                            source.columnLayers.cend());
    preview.layerHeight = stack->layerHeight;
    preview.layerCount = stack->layerCount;
    return mode == JobStackDisplayMode::Solid
        ? MaskStackMeshBuilder::buildSolid(preview, token, {})
        : MaskStackMeshBuilder::buildExploded(preview, token, {}, 100'000);
}

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
        void* mapped = nullptr;
    };

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer,
                      bool deviceLocal = false);
    void destroyBuffer(Buffer* buffer);
    bool upload(Buffer* buffer, const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    bool uploadStatic(Buffer* buffer, const void* data, VkDeviceSize size,
                      VkBufferUsageFlags usage, int frame);
    VkShaderModule loadShader(const QString& resourcePath);
    VkPipeline createPipeline(VkShaderModule vertex, VkShaderModule fragment,
                               VkPrimitiveTopology topology, bool pointPipeline);
    VkPipeline createStackPipeline(VkShaderModule vertex, VkShaderModule fragment);
    void rebuildPointBuffer();
    void rebuildStackBuffers(int frame);

    struct MeshBuffers {
        std::shared_ptr<const MaskStackGeometry> geometry;
        Buffer vertex;
        Buffer index;
        std::uint32_t indexCount = 0;
        std::size_t byteSize = 0;
        int lodLevel = -1;
    };
    struct StackBatch {
        const MeshBuffers* mesh = nullptr;
        Buffer instances;
        std::uint32_t instanceCount = 0;
    };

    JobVulkanWindow* window_ = nullptr;
    QVulkanDeviceFunctions* deviceFunctions_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipeline pointPipeline_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;
    VkPipeline stackPipeline_ = VK_NULL_HANDLE;
    Buffer pointBuffer_;
    std::uint32_t pointCount_ = 0;
    std::map<const MaskStackGeometry*, MeshBuffers> meshCache_;
    std::set<const MaskStackGeometry*> failedGpuMeshes_;
    std::vector<std::map<const MaskStackGeometry*, StackBatch>> stackFrames_;
    std::vector<quint64> stackFrameRevisions_;
    std::vector<std::vector<Buffer>> stagingFrames_;
    std::vector<Buffer> uniformBuffers_;
    std::vector<Buffer> lineBuffers_;
    std::vector<VkDescriptorSet> descriptorSets_;
    VkQueryPool timestampPool_ = VK_NULL_HANDLE;
    float timestampPeriod_ = 0.0F;
    std::uint64_t timestampMask_ = 0;
    std::size_t gpuMeshBytes_ = 0;
    quint64 uploadedDocumentGeneration_ = std::numeric_limits<quint64>::max();
    bool initialized_ = false;
};

class JobVulkanWindow final : public QVulkanWindow {
public:
    JobVulkanWindow() {
        setFlags(PersistentResources);
        lodPool_.setMaxThreadCount(2);
        interactionTimer_.setInterval(16);
        QObject::connect(&interactionTimer_, &QTimer::timeout, this, [this] { tickInteraction(); });
        settleTimer_.setSingleShot(true);
        settleTimer_.setInterval(200);
        QObject::connect(&settleTimer_, &QTimer::timeout, this, [this] {
            interacting_ = false;
            markStackBuffersDirty();
            recoveryFramesRemaining_ = 120;
            recoveryTimer_.start();
            requestUpdate();
        });
        recoveryTimer_.setTimerType(Qt::PreciseTimer);
        recoveryTimer_.setInterval(16);
        QObject::connect(&recoveryTimer_, &QTimer::timeout, this, [this] {
            if (interacting_ || recoveryFramesRemaining_-- <= 0) {
                recoveryTimer_.stop();
                return;
            }
            requestUpdate();
        });
    }

    ~JobVulkanWindow() override {
        if (lodToken_) lodToken_->cancel();
        lodPool_.clear();
        lodPool_.waitForDone();
    }

    QVulkanWindowRenderer* createRenderer() override {
        renderer_ = new JobVulkanRenderer(this);
        return renderer_;
    }

    void setDocument(const JobDocumentPtr& document) {
        if (lodToken_) lodToken_->cancel();
        lodToken_ = std::make_shared<CancellationToken>();
        ++documentGeneration_;
        lodCache_.clear();
        pendingLods_.clear();
        failedLods_.clear();
        lodCacheBytes_ = 0;
        lodUseSerial_ = 0;
        frameBudget_.reset();
        document_ = document;
        selectedId_ = hoveredId_ = -1;
        idToIndex_.clear();
        origin_ = {};
        if (document_ && document_->bounds.valid) {
            const JobBounds& scene = document_->bounds;
            origin_ = scene.center();
            idToIndex_.resize(document_->points.size(), -1);
            for (size_t index = 0; index < document_->points.size(); ++index) {
                const qint64 id = document_->points[index].placementId;
                if (id >= 0 && id < static_cast<qint64>(idToIndex_.size()))
                    idToIndex_[static_cast<size_t>(id)] = static_cast<qint64>(index);
            }
            camera_.setSceneBounds(scene.minimum.relativeTo(origin_),
                                   scene.maximum.relativeTo(origin_));
        } else {
            camera_.setSceneBounds({-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F});
        }
        animationActive_ = false;
        interacting_ = false;
        activeKeys_.clear();
        interactionTimer_.stop();
        recoveryTimer_.stop();
        recoveryFramesRemaining_ = 0;
        pointBufferDirty_ = true;
        markStackBuffersDirty();
        requestUpdate();
    }

    void setStackDisplayMode(JobStackDisplayMode mode) {
        if (stackMode_ == mode) return;
        stackMode_ = mode;
        markStackBuffersDirty();
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
    [[nodiscard]] qint64 pointIndexForPlacement(qint64 id) const {
        return id >= 0 && id < static_cast<qint64>(idToIndex_.size())
            ? idToIndex_[static_cast<std::size_t>(id)] : -1;
    }
    [[nodiscard]] bool takePointBufferDirty() { return std::exchange(pointBufferDirty_, false); }
    [[nodiscard]] quint64 stackBufferRevision() const noexcept { return stackBufferRevision_; }
    [[nodiscard]] JobStackDisplayMode stackDisplayMode() const noexcept { return stackMode_; }
    [[nodiscard]] bool interacting() const noexcept { return interacting_; }
    [[nodiscard]] JobFrameBudgetController& frameBudget() noexcept { return frameBudget_; }

    GeometrySelection geometryFor(const JobModelStackPtr& stack, int desiredLevel) {
        if (!stack) return {};
        if (stack->lodLevels.empty()) {
            auto geometry = stackMode_ == JobStackDisplayMode::Exploded
                ? stack->explodedGeometry : stack->solidGeometry;
            if (!geometry) geometry = stack->solidGeometry;
            return {geometry, 0};
        }
        desiredLevel = std::clamp(desiredLevel, 0,
                                  static_cast<int>(stack->lodLevels.size()) - 1);
        const LodCacheKey desired{stack.get(), stackMode_, desiredLevel};
        if (auto found = lodCache_.find(desired); found != lodCache_.end()) {
            found->second.lastUsed = ++lodUseSerial_;
            return {found->second.geometry, desiredLevel};
        }
        if (stackMode_ == JobStackDisplayMode::Solid && desiredLevel == 0 &&
            stack->solidGeometry) {
            insertLod(desired, stack->solidGeometry);
            return {stack->solidGeometry, 0};
        }
        scheduleLod(stack, desired);
        for (int level = desiredLevel - 1; level >= 0; --level) {
            const LodCacheKey fallback{stack.get(), stackMode_, level};
            if (auto found = lodCache_.find(fallback); found != lodCache_.end()) {
                found->second.lastUsed = ++lodUseSerial_;
                return {found->second.geometry, level};
            }
        }
        if (stack->solidGeometry) return {stack->solidGeometry, 0};
        return {};
    }

    void visiblePlacements(std::vector<quint32>* result, std::size_t maximum) const {
        if (!result) return;
        result->clear();
        if (!document_) return;
        if (document_->placementSpatialIndex)
            document_->placementSpatialIndex->queryVisible(
                viewProjection(), origin_, result, maximum);
        else {
            result->reserve(std::min(document_->points.size(), maximum));
            for (quint32 index = 0; index < document_->points.size(); ++index)
                if (document_->points[index].placeId >= 0) {
                    result->push_back(index);
                    if (result->size() >= maximum) break;
                }
        }
    }

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
        if (interacting_) appendPriorityLabels(lines, mvp);
        else appendLabels(lines, mvp);
        return lines;
    }

    std::function<void(qint64)> selectedCallback;
    std::function<void(qint64)> activatedCallback;
    std::function<void(double,std::size_t,std::size_t,qint64,int,int)> renderStatsCallback;

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
                noteInteraction();
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
        noteInteraction(); event->accept();
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
        if (selectedId_ >= 0 && activatedCallback) activatedCallback(selectedId_);
        requestUpdate();
        event->accept();
    }

private:
    friend class JobVulkanRenderer;

    void scheduleLod(const JobModelStackPtr& stack, const LodCacheKey& key) {
        if (!stack || activeLodBuilds_ >= 2 || pendingLods_.contains(key) ||
            failedLods_.contains(key) ||
            (lodToken_ && lodToken_->isCancelled())) return;
        pendingLods_.insert(key);
        ++activeLodBuilds_;
        const quint64 generation = documentGeneration_;
        const auto token = lodToken_;
        auto* watcher = new QFutureWatcher<LodBuildResult>(this);
        QObject::connect(watcher, &QFutureWatcher<LodBuildResult>::finished, this,
                         [this, watcher, key, generation] {
            const LodBuildResult result = watcher->result();
            watcher->deleteLater();
            pendingLods_.erase(key);
            activeLodBuilds_ = std::max(0, activeLodBuilds_ - 1);
            if (generation == documentGeneration_ && result.geometry) {
                if (insertLod(key, result.geometry)) {
                    markStackBuffersDirty();
                } else failedLods_.insert(key);
            } else if (generation == documentGeneration_ &&
                       lodToken_ && !lodToken_->isCancelled()) failedLods_.insert(key);
            requestUpdate();
        });
        watcher->setFuture(QtConcurrent::run(&lodPool_, [stack, key, token, generation] {
            return LodBuildResult{key,
                buildLodGeometry(stack, key.level, key.mode, token), generation};
        }));
    }

    void noteInteraction() {
        interacting_ = true;
        recoveryTimer_.stop();
        recoveryFramesRemaining_ = 0;
        settleTimer_.start();
        requestUpdate();
    }

    void markStackBuffersDirty() {
        // Camera interaction reuses the last complete batches. Any model/mode/LOD
        // changes are folded into the single rebuild scheduled by settleTimer_.
        if (interacting_) return;
        ++stackBufferRevision_;
        if (stackBufferRevision_ == 0) ++stackBufferRevision_;
    }

    bool insertLod(const LodCacheKey& key,
                   const std::shared_ptr<const MaskStackGeometry>& geometry) {
        if (!geometry) return false;
        constexpr std::size_t maximumBytes = 256U * 1024U * 1024U;
        const std::size_t bytes = geometry->vertices.size() * sizeof(MaskStackVertex) +
                                  geometry->indices.size() * sizeof(std::uint32_t);
        if (bytes > maximumBytes) return false;
        if (auto existing = lodCache_.find(key); existing != lodCache_.end()) {
            existing->second.lastUsed = ++lodUseSerial_;
            return true;
        }
        while (lodCacheBytes_ + bytes > maximumBytes && !lodCache_.empty()) {
            auto victim = lodCache_.end();
            for (auto candidate = lodCache_.begin(); candidate != lodCache_.end(); ++candidate) {
                if (candidate->first.stack == key.stack && candidate->first.mode == key.mode &&
                    candidate->first.level == key.level) continue;
                if (victim == lodCache_.end() ||
                    candidate->second.lastUsed < victim->second.lastUsed ||
                    (candidate->second.lastUsed == victim->second.lastUsed &&
                     candidate->first.level > victim->first.level)) victim = candidate;
            }
            if (victim == lodCache_.end()) break;
            lodCacheBytes_ -= std::min(lodCacheBytes_, victim->second.byteSize);
            lodCache_.erase(victim);
        }
        if (lodCacheBytes_ + bytes > maximumBytes) return false;
        lodCache_[key] = {geometry, bytes, ++lodUseSerial_};
        lodCacheBytes_ += bytes;
        return true;
    }

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
        return camera_.groundIntersection(screen, QSizeF(width(), height()),
                                           viewProjection(), intersection);
    }

    void panOnGround(const QPointF& previous, const QPointF& current) {
        camera_.panOnGround(previous, current, QSizeF(width(), height()), viewProjection());
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
        if (changed) noteInteraction();
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

    void appendPriorityLabels(std::vector<LineVertex>& lines, const QMatrix4x4& mvp) const {
        const auto add = [&](qint64 id, const QVector3D& color) {
            if (id < 0 || id >= static_cast<qint64>(idToIndex_.size())) return;
            const qint64 index = idToIndex_[static_cast<std::size_t>(id)];
            if (index < 0) return;
            QPointF screen;
            if (project(localPosition(document_->points[static_cast<std::size_t>(index)]),
                        mvp, &screen))
                appendText(lines, screen, QString::number(id), color);
        };
        add(hoveredId_, {1.0F, 0.50F, 0.15F});
        add(selectedId_, {1.0F, 0.85F, 0.0F});
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
        if (!interacting_ && lod.level == JobLabelLodLevel::Individual) {
            for (const ProjectedPoint& point : projected) {
                const qint64 id=document_->points[point.pointIndex].placementId;
                if (id == selectedId_ || id == hoveredId_) continue;
                addOrdinary(point.screen, QString::number(id));
            }
        } else if (!interacting_ && lod.level == JobLabelLodLevel::Range) {
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
        appendPriorityLabels(lines, mvp);
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
    quint64 stackBufferRevision_ = 1;
    JobStackDisplayMode stackMode_ = JobStackDisplayMode::Solid;
    Qt::MouseButton pressedButton_ = Qt::NoButton;
    QPointF pressPosition_;
    QPointF lastPosition_;
    bool dragging_ = false;
    mutable QElapsedTimer hoverTimer_;
    QElapsedTimer statsTimer_;
    QTimer settleTimer_;
    QTimer recoveryTimer_;
    QThreadPool lodPool_;
    CancellationTokenPtr lodToken_ = std::make_shared<CancellationToken>();
    std::map<LodCacheKey, LodCacheEntry> lodCache_;
    std::set<LodCacheKey> pendingLods_;
    std::set<LodCacheKey> failedLods_;
    std::size_t lodCacheBytes_ = 0;
    quint64 lodUseSerial_ = 0;
    JobFrameBudgetController frameBudget_;
    quint64 documentGeneration_ = 0;
    int activeLodBuilds_ = 0;
    int recoveryFramesRemaining_ = 0;
    bool interacting_ = false;
    std::size_t lastVisibleCount_ = 0;
    std::size_t lastProxyCount_ = 0;
    qint64 lastTriangleCount_ = 0;
    int lastMinimumLod_ = -1;
    int lastMaximumLod_ = -1;
};

bool JobVulkanRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer,
                                     bool deviceLocal) {
    if (!buffer || size == 0) return false;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (deviceFunctions_->vkCreateBuffer(device_, &info, nullptr, &buffer->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    deviceFunctions_->vkGetBufferMemoryRequirements(device_, buffer->buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = deviceLocal ? window_->deviceLocalMemoryIndex()
                                             : window_->hostVisibleMemoryIndex();
    if (deviceFunctions_->vkAllocateMemory(device_, &allocation, nullptr, &buffer->memory) != VK_SUCCESS) {
        deviceFunctions_->vkDestroyBuffer(device_, buffer->buffer, nullptr); buffer->buffer = VK_NULL_HANDLE; return false;
    }
    if (deviceFunctions_->vkBindBufferMemory(device_, buffer->buffer, buffer->memory, 0) != VK_SUCCESS) {
        destroyBuffer(buffer); return false;
    }
    if (!deviceLocal) {
        if (deviceFunctions_->vkMapMemory(device_, buffer->memory, 0, VK_WHOLE_SIZE, 0,
                                          &buffer->mapped) != VK_SUCCESS) {
            destroyBuffer(buffer); return false;
        }
    }
    buffer->capacity = size;
    buffer->allocationSize = requirements.size;
    return true;
}

void JobVulkanRenderer::destroyBuffer(Buffer* buffer) {
    if (!buffer || !deviceFunctions_) return;
    if (buffer->mapped && buffer->memory)
        deviceFunctions_->vkUnmapMemory(device_, buffer->memory);
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
    if (!buffer->mapped) return false;
    std::memcpy(buffer->mapped, data, static_cast<size_t>(size));
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = buffer->memory; range.offset = 0; range.size = VK_WHOLE_SIZE;
    deviceFunctions_->vkFlushMappedMemoryRanges(device_, 1, &range);
    return true;
}

bool JobVulkanRenderer::uploadStatic(Buffer* buffer, const void* data, VkDeviceSize size,
                                     VkBufferUsageFlags usage, int frame) {
    if (!buffer || !data || size == 0 || frame < 0 ||
        frame >= static_cast<int>(stagingFrames_.size())) return false;
    destroyBuffer(buffer);
    if (!createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, buffer, true)) return false;
    Buffer staging;
    if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging) ||
        !upload(&staging, data, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        destroyBuffer(&staging); destroyBuffer(buffer); return false;
    }
    VkBufferCopy copy{0,0,size};
    VkCommandBuffer command = window_->currentCommandBuffer();
    deviceFunctions_->vkCmdCopyBuffer(command, staging.buffer, buffer->buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT
        ? VK_ACCESS_INDEX_READ_BIT : VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer->buffer;
    barrier.offset = 0;
    barrier.size = size;
    deviceFunctions_->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
    stagingFrames_[frame].push_back(std::move(staging));
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

VkPipeline JobVulkanRenderer::createStackPipeline(VkShaderModule vertex,
                                                   VkShaderModule fragment) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    const VkVertexInputBindingDescription bindings[2]{
        {0, sizeof(MaskStackVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(StackInstance), VK_VERTEX_INPUT_RATE_INSTANCE}};
    const VkVertexInputAttributeDescription attributes[8]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32_SFLOAT, 24},
        {3, 0, VK_FORMAT_R32_SFLOAT, 28},
        {4, 1, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {5, 1, VK_FORMAT_R32_SFLOAT, 12},
        {6, 1, VK_FORMAT_R32_SFLOAT, 16},
        {7, 1, VK_FORMAT_R32G32B32_SFLOAT, 20}};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 2;
    input.pVertexBindingDescriptions = bindings;
    input.vertexAttributeDescriptionCount = 8;
    input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = window_->sampleCountFlagBits();
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout_;
    info.renderPass = window_->defaultRenderPass();
    VkPipeline pipeline = VK_NULL_HANDLE;
    return deviceFunctions_->vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info,
                                                        nullptr, &pipeline) == VK_SUCCESS
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
    stackFrames_.resize(frames);
    stackFrameRevisions_.resize(frames);
    stagingFrames_.resize(frames);
    VkPhysicalDeviceProperties physicalProperties{};
    QVulkanFunctions* instanceFunctions = window_->vulkanInstance()->functions();
    instanceFunctions->vkGetPhysicalDeviceProperties(
        window_->physicalDevice(), &physicalProperties);
    timestampPeriod_ = physicalProperties.limits.timestampPeriod;
    std::uint32_t queueFamilyCount = 0;
    instanceFunctions->vkGetPhysicalDeviceQueueFamilyProperties(
        window_->physicalDevice(), &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    instanceFunctions->vkGetPhysicalDeviceQueueFamilyProperties(
        window_->physicalDevice(), &queueFamilyCount, queueFamilies.data());
    const std::uint32_t family = window_->graphicsQueueFamilyIndex();
    const std::uint32_t validBits = family < queueFamilies.size()
        ? queueFamilies[family].timestampValidBits : 0;
    timestampMask_ = validBits == 0 ? 0
        : validBits >= 64 ? std::numeric_limits<std::uint64_t>::max()
                          : (std::uint64_t{1} << validBits) - 1;
    if (timestampPeriod_ > 0.0F && timestampMask_ != 0) {
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = static_cast<std::uint32_t>(frames * 2);
        if (deviceFunctions_->vkCreateQueryPool(device_, &queryInfo, nullptr,
                                                 &timestampPool_) != VK_SUCCESS)
            timestampPool_ = VK_NULL_HANDLE;
    }
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
    pointCount_ = 0;
    const auto& document = window_->document();
    if (!document || document->points.empty()) return;
    std::vector<PointVertex> vertices;
    const std::size_t maximumPoints = document->points.size();
    const std::size_t stride = std::max<std::size_t>(1,
        (document->points.size() + maximumPoints - 1) / maximumPoints);
    vertices.reserve(std::min(document->points.size(), maximumPoints));
    const JobCoordinate origin = window_->origin();
    for (std::size_t pointIndex = 0; pointIndex < document->points.size(); pointIndex += stride) {
        const JobPoint& point = document->points[pointIndex];
        const QVector3D local = point.position.relativeTo(origin);
        QColor color = point.groupColor.isValid() ? point.groupColor : QColor(26, 191, 255);
        vertices.push_back({local.x(), local.y(), local.z(),
                            static_cast<float>(point.placementId), color.redF(),
                            color.greenF(), color.blueF()});
    }
    if (upload(&pointBuffer_, vertices.data(), vertices.size() * sizeof(PointVertex),
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        pointCount_ = static_cast<std::uint32_t>(vertices.size());
}

void JobVulkanRenderer::rebuildStackBuffers(int frame) {
    if (frame < 0 || frame >= static_cast<int>(stackFrames_.size()) ||
        frame >= static_cast<int>(stackFrameRevisions_.size())) return;
    for (auto& staging : stagingFrames_[frame]) destroyBuffer(&staging);
    stagingFrames_[frame].clear();
    if (uploadedDocumentGeneration_ != window_->documentGeneration_) {
        deviceFunctions_->vkDeviceWaitIdle(device_);
        for (auto& bufferedFrame : stackFrames_) {
            for (auto& [key, batch] : bufferedFrame) {
                Q_UNUSED(key);
                destroyBuffer(&batch.instances);
            }
            bufferedFrame.clear();
        }
        for (auto& [key, mesh] : meshCache_) {
            Q_UNUSED(key);
            destroyBuffer(&mesh.vertex); destroyBuffer(&mesh.index);
        }
        meshCache_.clear(); failedGpuMeshes_.clear(); gpuMeshBytes_ = 0;
        std::fill(stackFrameRevisions_.begin(), stackFrameRevisions_.end(), 0);
        uploadedDocumentGeneration_ = window_->documentGeneration_;
    }
    const quint64 revision = window_->stackBufferRevision();
    if (stackFrameRevisions_[frame] == revision) return;
    auto& frameBatches = stackFrames_[frame];
    for (auto& [key, batch] : frameBatches) {
        Q_UNUSED(key);
        batch.instanceCount = 0;
    }
    const auto& document = window_->document();
    if (!document || document->loadedStackCount() <= 0) {
        stackFrameRevisions_[frame] = revision;
        return;
    }

    struct Candidate {
        quint32 pointIndex = 0;
        JobModelStackPtr stack;
        double projectedWidth = 0.0;
        double projectedHeight = 0.0;
        double area = 0.0;
        int priority = 0;
    };
    const QSizeF viewport(std::max(1, window_->width()), std::max(1, window_->height()));
    const qint64 triangleBudget = window_->frameBudget().triangleBudget();
    const std::size_t viewportLimit = static_cast<std::size_t>(std::clamp(
        viewport.width() * viewport.height() / (window_->interacting() ? 12.0 : 6.0),
        10'000.0, window_->interacting() ? 20'000.0 : 40'000.0));
    const std::size_t budgetLimit = static_cast<std::size_t>(std::max<qint64>(
        1'000, triangleBudget / 12));
    const std::size_t processingLimit = std::min(viewportLimit, budgetLimit);
    std::vector<quint32> visible;
    window_->visiblePlacements(&visible, processingLimit);
    const std::size_t visiblePlacementCount = visible.size();
    std::vector<quint32> priorityPlacements;
    for (qint64 id : {window_->selectedId(), window_->hoveredId()}) {
        const qint64 pointIndex = window_->pointIndexForPlacement(id);
        if (pointIndex >= 0)
            priorityPlacements.push_back(static_cast<quint32>(pointIndex));
    }
    for (quint32 priority : priorityPlacements) {
        if (std::find(visible.cbegin(), visible.cend(), priority) != visible.cend()) continue;
        if (!visible.empty()) visible.back() = priority;
        else visible.push_back(priority);
    }
    std::vector<Candidate> candidates;
    candidates.reserve(visible.size());
    const QMatrix4x4 mvp = window_->viewProjection();
    const JobCoordinate origin = window_->origin();
    for (quint32 pointIndex : visible) {
        if (pointIndex >= document->points.size()) continue;
        const JobPoint& point = document->points[pointIndex];
        if (point.placeId < 0 || point.placeId >= static_cast<qint64>(document->models.size())) continue;
        const JobModel& model = document->models[static_cast<std::size_t>(point.placeId)];
        if (!model.stack) continue;
        const QVector3D base = point.position.relativeTo(origin);
        const QVector3D extent(static_cast<float>(model.stack->physicalSize.width()),
                               static_cast<float>(model.stack->physicalSize.height()),
                               static_cast<float>(model.stack->layerCount * model.stack->layerHeight));
        double minimumX = std::numeric_limits<double>::max();
        double minimumY = std::numeric_limits<double>::max();
        double maximumX = std::numeric_limits<double>::lowest();
        double maximumY = std::numeric_limits<double>::lowest();
        int projected = 0;
        for (int mask = 0; mask < 8; ++mask) {
            QPointF screen;
            const QVector3D corner = base + QVector3D((mask & 1) ? extent.x() : 0.0F,
                                                       (mask & 2) ? extent.y() : 0.0F,
                                                       (mask & 4) ? extent.z() : 0.0F);
            if (!JobPreviewMath::project(corner, mvp, viewport, &screen)) continue;
            minimumX = std::min(minimumX, screen.x()); maximumX = std::max(maximumX, screen.x());
            minimumY = std::min(minimumY, screen.y()); maximumY = std::max(maximumY, screen.y());
            ++projected;
        }
        const double projectedWidth = projected ? std::max(1.0, maximumX - minimumX)
                                                : viewport.width();
        const double projectedHeight = projected ? std::max(1.0, maximumY - minimumY)
                                                 : viewport.height();
        const int priority = point.placementId == window_->selectedId() ? 2
                           : point.placementId == window_->hoveredId() ? 1 : 0;
        candidates.push_back({pointIndex, model.stack, projectedWidth, projectedHeight,
                              projectedWidth * projectedHeight, priority});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                               const Candidate& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        return left.area > right.area;
    });

    struct PendingBatch {
        std::shared_ptr<const MaskStackGeometry> geometry;
        std::vector<StackInstance> instances;
        int lodLevel = -1;
    };
    std::map<const MaskStackGeometry*, PendingBatch> batches;
    qint64 usedTriangles = 0;
    std::size_t proxyCount = 0;
    int minimumLod = std::numeric_limits<int>::max();
    int maximumLod = -1;
    for (const Candidate& candidate : candidates) {
        const JobPoint& point = document->points[candidate.pointIndex];
        int desiredLevel = JobStackLod::chooseLevel(candidate.stack->lodLevels,
            candidate.projectedWidth, candidate.projectedHeight, window_->interacting(),
            window_->frameBudget().qualityOffset() + (candidate.priority > 0 ? 1 : 0));
        GeometrySelection selection = window_->geometryFor(candidate.stack, desiredLevel);
        qint64 triangles = selection.geometry
            ? static_cast<qint64>(selection.geometry->indices.size() / 3) : 0;
        constexpr std::size_t maximumGpuMeshBytes = 256U * 1024U * 1024U;
        const auto gpuMeshUnavailable = [&](const GeometrySelection& value) {
            if (!value.geometry) return true;
            const std::size_t bytes = value.geometry->vertices.size() * sizeof(MaskStackVertex) +
                                      value.geometry->indices.size() * sizeof(std::uint32_t);
            return failedGpuMeshes_.contains(value.geometry.get()) ||
                (!meshCache_.contains(value.geometry.get()) &&
                  gpuMeshBytes_ + bytes > maximumGpuMeshBytes);
        };
        if (selection.geometry && candidate.priority == 0 &&
            (gpuMeshUnavailable(selection) ||
             usedTriangles + triangles > triangleBudget)) {
            for (int lowerLevel = selection.level - 1; lowerLevel >= 0; --lowerLevel) {
                GeometrySelection lower = window_->geometryFor(candidate.stack, lowerLevel);
                if (!lower.geometry) continue;
                const qint64 lowerTriangles = static_cast<qint64>(lower.geometry->indices.size() / 3);
                selection = std::move(lower);
                triangles = lowerTriangles;
                if (!gpuMeshUnavailable(selection) &&
                    usedTriangles + triangles <= triangleBudget) break;
            }
        }
        const bool unavailable = !selection.geometry || gpuMeshUnavailable(selection) ||
            (candidate.priority == 0 && usedTriangles + triangles > triangleBudget);
        if (unavailable) {
            ++proxyCount;
            continue;
        }
        const auto geometry = selection.geometry;
        if (!geometry || geometry->vertices.empty() || geometry->indices.empty()) continue;
        PendingBatch& batch = batches[geometry.get()];
        batch.geometry = geometry;
        batch.lodLevel = selection.level;
        const QVector3D local = point.position.relativeTo(origin);
        batch.instances.push_back({local.x(), local.y(), local.z(),
                                   static_cast<float>(point.placementId),
                                   static_cast<float>(candidate.stack->layerCount),
                                   1.0F, 1.0F, 1.0F});
        usedTriangles += triangles;
        minimumLod = std::min(minimumLod, selection.level);
        maximumLod = std::max(maximumLod, selection.level);
    }
    const auto makeGpuRoom = [&](std::size_t bytes) {
        constexpr std::size_t maximumBytes = 256U * 1024U * 1024U;
        return gpuMeshBytes_ + bytes <= maximumBytes;
    };
    for (const auto& [key, pending] : batches) {
        auto mesh = meshCache_.find(key);
        if (mesh == meshCache_.end()) {
            if (failedGpuMeshes_.contains(key)) continue;
            MeshBuffers created;
            created.geometry = pending.geometry;
            const auto& geometry = *pending.geometry;
            const std::size_t meshBytes = geometry.vertices.size() * sizeof(MaskStackVertex) +
                                          geometry.indices.size() * sizeof(std::uint32_t);
            if (!makeGpuRoom(meshBytes)) continue;
            if (!uploadStatic(&created.vertex, geometry.vertices.data(),
                        geometry.vertices.size() * sizeof(MaskStackVertex),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, frame) ||
                !uploadStatic(&created.index, geometry.indices.data(),
                        geometry.indices.size() * sizeof(std::uint32_t),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, frame)) {
                destroyBuffer(&created.vertex); destroyBuffer(&created.index);
                failedGpuMeshes_.insert(key);
                continue;
            }
            created.indexCount = static_cast<std::uint32_t>(std::min<std::size_t>(
                geometry.indices.size(), std::numeric_limits<std::uint32_t>::max()));
            created.byteSize = meshBytes;
            created.lodLevel = pending.lodLevel;
            mesh = meshCache_.emplace(key, std::move(created)).first;
            gpuMeshBytes_ += meshBytes;
        }
        StackBatch& batch = frameBatches[key];
        batch.mesh = &mesh->second;
        const auto& geometry = *pending.geometry;
        Q_UNUSED(geometry);
        if (!upload(&batch.instances, pending.instances.data(),
                    pending.instances.size() * sizeof(StackInstance),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            batch.instanceCount = 0;
            continue;
        }
        batch.instanceCount = static_cast<std::uint32_t>(std::min<std::size_t>(
            pending.instances.size(), std::numeric_limits<std::uint32_t>::max()));
    }
    window_->lastVisibleCount_ = visiblePlacementCount;
    window_->lastProxyCount_ = proxyCount;
    window_->lastTriangleCount_ = usedTriangles;
    window_->lastMinimumLod_ = minimumLod == std::numeric_limits<int>::max() ? -1 : minimumLod;
    window_->lastMaximumLod_ = maximumLod;
    stackFrameRevisions_[frame] = revision;
}

void JobVulkanRenderer::startNextFrame() {
    if (!initialized_) { window_->frameReady(); return; }
    QElapsedTimer cpuFrameTimer;
    cpuFrameTimer.start();
    const int frame = window_->currentFrame();
    bool recordedGpuTime = false;
    double gpuMilliseconds = 0.0;
    if (timestampPool_) {
        std::uint64_t timestamps[2]{};
        if (deviceFunctions_->vkGetQueryPoolResults(device_, timestampPool_, frame * 2, 2,
                sizeof(timestamps), timestamps, sizeof(std::uint64_t),
                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const std::uint64_t elapsedTicks = (timestamps[1] - timestamps[0]) & timestampMask_;
            const double milliseconds = double(elapsedTicks) *
                                        timestampPeriod_ / 1'000'000.0;
            gpuMilliseconds = milliseconds;
            recordedGpuTime = true;
        }
    }
    if (window_->takePointBufferDirty()) rebuildPointBuffer();
    CameraUniform uniform;
    const QMatrix4x4 matrix = window_->viewProjection();
    std::memcpy(uniform.mvp, matrix.constData(), sizeof(uniform.mvp));
    uniform.selectedId = static_cast<float>(window_->selectedId());
    uniform.hoveredId = static_cast<float>(window_->hoveredId());
    uniform.viewportSize[0]=static_cast<float>(std::max(1,window_->swapChainImageSize().width()));
    uniform.viewportSize[1]=static_cast<float>(std::max(1,window_->swapChainImageSize().height()));
    uniform.stackExplode = window_->stackDisplayMode() == JobStackDisplayMode::Exploded ? 1.2F : 1.0F;
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
    if (timestampPool_) {
        deviceFunctions_->vkCmdResetQueryPool(command, timestampPool_, frame * 2, 2);
        deviceFunctions_->vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                               timestampPool_, frame * 2);
    }
    deviceFunctions_->vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, float(begin.renderArea.extent.width), float(begin.renderArea.extent.height), 0, 1};
    VkRect2D scissor{{0,0}, begin.renderArea.extent};
    deviceFunctions_->vkCmdSetViewport(command, 0, 1, &viewport);
    deviceFunctions_->vkCmdSetScissor(command, 0, 1, &scissor);
    const auto& document = window_->document();
    deviceFunctions_->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                               pipelineLayout_, 0, 1,
                                               &descriptorSets_[frame], 0, nullptr);
    if (document && pointBuffer_.buffer) {
        const VkDeviceSize offset = 0;
        deviceFunctions_->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pointPipeline_);
        deviceFunctions_->vkCmdBindVertexBuffers(command, 0, 1, &pointBuffer_.buffer, &offset);
        deviceFunctions_->vkCmdDraw(command, 6, pointCount_, 0, 0);
    }
    if (!lines.empty() && lineBuffers_[frame].buffer) {
        const VkDeviceSize offset = 0;
        deviceFunctions_->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);
        deviceFunctions_->vkCmdBindVertexBuffers(command, 0, 1, &lineBuffers_[frame].buffer, &offset);
        deviceFunctions_->vkCmdDraw(command, static_cast<uint32_t>(lines.size()), 1, 0, 0);
    }
    deviceFunctions_->vkCmdEndRenderPass(command);
    if (timestampPool_)
        deviceFunctions_->vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                               timestampPool_, frame * 2 + 1);
    const double cpuMilliseconds = cpuFrameTimer.nsecsElapsed() / 1'000'000.0;
    const double measuredMilliseconds = std::max(cpuMilliseconds,
        recordedGpuTime ? gpuMilliseconds : 0.0);
    const qint64 previousBudget = window_->frameBudget().triangleBudget();
    const int previousQuality = window_->frameBudget().qualityOffset();
    window_->frameBudget().recordFrame(measuredMilliseconds, window_->interacting());
    if (!window_->interacting() &&
        (previousBudget != window_->frameBudget().triangleBudget() ||
         previousQuality != window_->frameBudget().qualityOffset()))
        window_->markStackBuffersDirty();
    if (window_->renderStatsCallback &&
        (!window_->statsTimer_.isValid() || window_->statsTimer_.elapsed() >= 250)) {
        window_->statsTimer_.restart();
        window_->renderStatsCallback(window_->frameBudget().framesPerSecond(),
            window_->lastVisibleCount_, window_->lastProxyCount_, window_->lastTriangleCount_,
            window_->lastMinimumLod_, window_->lastMaximumLod_);
    }
    window_->frameReady();
}

void JobVulkanRenderer::releaseResources() {
    if (!deviceFunctions_) return;
    for (auto& buffer : lineBuffers_) destroyBuffer(&buffer);
    for (auto& buffer : uniformBuffers_) destroyBuffer(&buffer);
    for (auto& frame : stackFrames_)
        for (auto& [key, batch] : frame) {
            Q_UNUSED(key);
            destroyBuffer(&batch.instances);
        }
    stackFrames_.clear();
    for (auto& frame : stagingFrames_)
        for (auto& staging : frame) destroyBuffer(&staging);
    stagingFrames_.clear();
    for (auto& [key, mesh] : meshCache_) {
        Q_UNUSED(key);
        destroyBuffer(&mesh.vertex);
        destroyBuffer(&mesh.index);
    }
    meshCache_.clear();
    destroyBuffer(&pointBuffer_);
    if (timestampPool_) deviceFunctions_->vkDestroyQueryPool(device_, timestampPool_, nullptr);
    if (pointPipeline_) deviceFunctions_->vkDestroyPipeline(device_, pointPipeline_, nullptr);
    if (linePipeline_) deviceFunctions_->vkDestroyPipeline(device_, linePipeline_, nullptr);
    if (stackPipeline_) deviceFunctions_->vkDestroyPipeline(device_, stackPipeline_, nullptr);
    if (pipelineLayout_) deviceFunctions_->vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorPool_) deviceFunctions_->vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (descriptorLayout_) deviceFunctions_->vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    pointPipeline_ = linePipeline_ = stackPipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE; descriptorPool_ = VK_NULL_HANDLE; descriptorLayout_ = VK_NULL_HANDLE;
    initialized_ = false; deviceFunctions_ = nullptr; device_ = VK_NULL_HANDLE;
    timestampPool_ = VK_NULL_HANDLE; timestampPeriod_ = 0.0F; timestampMask_ = 0;
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
        window_->activatedCallback = [this](qint64 id) {
            Q_EMIT owner_->placeActivated(id);
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
    statusLabel_->setToolTip(QString{});
    statusLabel_->setStyleSheet(QString{});
    if (!document_) statusLabel_->setText(QStringLiteral("未加载 Job"));
    else statusLabel_->setText(QStringLiteral("%1 个点 | %2")
        .arg(document_->points.size())
        .arg(document_->source == JobDocumentSource::ImportedFile
                 ? QStringLiteral("双击坐标查看堆叠") : QStringLiteral("分割联动")));
}

bool JobPreviewWidget::isVulkanAvailable() const noexcept { return backend_ && backend_->available(); }

void JobPreviewWidget::setSelectedPlaceId(qint64 placeId, bool focus, bool preserveZoom) {
    selectedPlaceId_ = placeId;
    backend_->setSelected(placeId, focus, preserveZoom);
}

void JobPreviewWidget::clearSelection() { setSelectedPlaceId(-1, false); }
void JobPreviewWidget::fitAll() { if (backend_) backend_->fitAll(); }
QImage JobPreviewWidget::grabFrame() { return backend_ ? backend_->grabFrame() : QImage(); }

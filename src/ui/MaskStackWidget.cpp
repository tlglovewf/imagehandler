#include "ui/MaskStackWidget.h"

#include "imaging/JobPreviewMath.h"

#include <QEvent>
#include <QFile>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QShortcut>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
#include <QMatrix4x4>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QVulkanWindow>

namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kExplodedSpacing = 1.2F;

struct StackLineVertex { float x, y, z, r, g, b; };
struct alignas(16) StackUniform {
    float mvp[16];
    float origin[4]{};
    float parameters[4]{1.0F, 0.1F, 1.0F, kExplodedSpacing};
    float startColor[4]{0.0F,0.0F,1.0F,1.0F};
    float endColor[4]{0.0F,1.0F,0.0F,1.0F};
    float selection[4]{1.0F,0.0F,0.0F,0.0F};
};
static_assert(sizeof(MaskStackVertex) == 32);
static_assert(sizeof(StackUniform) == 144);

class StackVulkanWindow;

class StackVulkanRenderer final : public QVulkanWindowRenderer {
public:
    explicit StackVulkanRenderer(StackVulkanWindow* window) : window_(window) {}
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
    };
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer);
    void destroyBuffer(Buffer* buffer);
    bool upload(Buffer* buffer, const void* data, VkDeviceSize size,
                VkBufferUsageFlags usage);
    VkShaderModule loadShader(const QString& path);
    VkPipeline createPipeline(VkShaderModule vertex, VkShaderModule fragment,
                              bool lines);
    void rebuildGeometry();
    void rebuildLines();

    StackVulkanWindow* window_ = nullptr;
    QVulkanDeviceFunctions* functions_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipeline surfacePipeline_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;
    Buffer vertexBuffer_;
    Buffer indexBuffer_;
    Buffer lineBuffer_;
    std::vector<Buffer> uniformBuffers_;
    std::vector<VkDescriptorSet> descriptorSets_;
    std::uint32_t indexCount_ = 0;
    std::uint32_t lineCount_ = 0;
    bool initialized_ = false;
};

class StackVulkanWindow final : public QVulkanWindow {
public:
    StackVulkanWindow() { camera_.setContinuousOrbitEnabled(true); }
    QVulkanWindowRenderer* createRenderer() override {
        renderer_ = new StackVulkanRenderer(this);
        return renderer_;
    }

    void setPreview(const MaskStackPreviewDataPtr& preview) {
        {
            QMutexLocker lock(&mutex_);
            preview_ = preview;
            geometryDirty_ = true;
            rebuildLinesLocked();
        }
        updateSceneBounds();
        requestUpdate();
    }
    void setColors(const QColor& start, const QColor& end) {
        QMutexLocker lock(&mutex_);
        startColor_ = start;
        endColor_ = end;
        requestUpdate();
    }
    void setMode(MaskStackDisplayMode mode) {
        {
            QMutexLocker lock(&mutex_);
            if (mode_ == mode) return;
            mode_ = mode;
            geometryDirty_ = true;
            rebuildLinesLocked();
        }
        updateSceneBounds();
        requestUpdate();
    }
    void setSelectedLayer(int layer) {
        QMutexLocker lock(&mutex_);
        selectedLayer_ = std::max(1, layer);
        rebuildLinesLocked();
        requestUpdate();
    }
    void setActiveBlock(qint64 traversalIndex) {
        QMutexLocker lock(&mutex_);
        activeBlock_ = traversalIndex;
        rebuildLinesLocked();
        requestUpdate();
    }
    void setZScale(double scale) {
        {
            QMutexLocker lock(&mutex_);
            zScale_ = static_cast<float>(std::clamp(scale, 1.0, 100.0));
            rebuildLinesLocked();
        }
        updateSceneBounds();
        requestUpdate();
    }
    void fitAll() {
        camera_.setPose(camera_.fittedPose());
        requestUpdate();
    }
    void viewTop() { setPreset(-90.0F, 89.0F); }
    void viewFront() { setPreset(-90.0F, 12.0F); }
    void viewRight() { setPreset(0.0F, 12.0F); }

    bool takeGeometry(std::shared_ptr<const MaskStackGeometry>* geometry) {
        QMutexLocker lock(&mutex_);
        if (!geometryDirty_) return false;
        geometryDirty_ = false;
        *geometry = !preview_ ? std::shared_ptr<const MaskStackGeometry>{}
            : mode_ == MaskStackDisplayMode::Solid ? preview_->solidGeometry
                                                   : preview_->explodedGeometry;
        return true;
    }
    bool takeLines(std::vector<StackLineVertex>* lines) {
        QMutexLocker lock(&mutex_);
        if (!linesDirty_) return false;
        linesDirty_ = false;
        *lines = lines_;
        return true;
    }
    StackUniform uniform() const {
        QMutexLocker lock(&mutex_);
        StackUniform result;
        const QMatrix4x4 matrix = viewProjection();
        std::memcpy(result.mvp, matrix.constData(), sizeof(result.mvp));
        result.origin[0] = origin_.x();
        result.origin[1] = origin_.y();
        result.origin[2] = origin_.z();
        result.parameters[0] = zScale_;
        if (preview_) {
            result.parameters[1] = static_cast<float>(preview_->layerHeight);
            result.parameters[2] = static_cast<float>(preview_->layerCount);
        }
        result.startColor[0] = startColor_.redF();
        result.startColor[1] = startColor_.greenF();
        result.startColor[2] = startColor_.blueF();
        result.endColor[0] = endColor_.redF();
        result.endColor[1] = endColor_.greenF();
        result.endColor[2] = endColor_.blueF();
        result.selection[0] = static_cast<float>(selectedLayer_);
        result.selection[1] = mode_ == MaskStackDisplayMode::Exploded ? 1.0F : 0.0F;
        return result;
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton &&
            event->button() != Qt::MiddleButton) return;
        pressedButton_ = event->button();
        lastPosition_ = event->position();
        requestActivate();
        setMouseGrabEnabled(true);
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (pressedButton_ == Qt::NoButton || !event->buttons().testFlag(pressedButton_)) return;
        const QPointF delta = event->position() - lastPosition_;
        if (pressedButton_ == Qt::LeftButton) camera_.orbit(delta);
        else {
            const float scale = worldPerPixel();
            camera_.translate(camera_.right() * float(-delta.x()) * scale +
                              camera_.groundForward() * float(delta.y()) * scale);
        }
        lastPosition_ = event->position();
        requestUpdate();
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == pressedButton_) {
            cancelPointerInteraction();
        }
        event->accept();
    }
    void wheelEvent(QWheelEvent* event) override {
        camera_.zoomBy(std::exp(-float(event->angleDelta().y()) * 0.0012F));
        requestUpdate();
        event->accept();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Space) {
            fitAll();
            event->accept();
            return;
        }
        QVulkanWindow::keyPressEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override {
        cancelPointerInteraction();
        QVulkanWindow::focusOutEvent(event);
    }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::UngrabMouse) pressedButton_ = Qt::NoButton;
        else if (event->type() == QEvent::Hide ||
                 event->type() == QEvent::WindowDeactivate) cancelPointerInteraction();
        return QVulkanWindow::event(event);
    }

private:
    friend class StackVulkanRenderer;
    void cancelPointerInteraction() {
        pressedButton_ = Qt::NoButton;
        setMouseGrabEnabled(false);
    }
    void setPreset(float azimuth, float elevation) {
        JobCameraPose pose = camera_.fittedPose();
        pose.azimuth = azimuth;
        pose.elevation = elevation;
        camera_.setPose(pose);
        requestUpdate();
    }
    float worldPerPixel() const {
        return std::max(0.000001F, 2.0F * camera_.pose().distance *
            std::tan(22.5F * kPi / 180.0F) / std::max(1, height()));
    }
    QMatrix4x4 viewProjection() const {
        const QSize renderSize = swapChainImageSize().isValid() ? swapChainImageSize() : size();
        const float aspect = renderSize.height() > 0 ?
            float(renderSize.width()) / renderSize.height() : 1.0F;
        QMatrix4x4 projection;
        projection.perspective(45.0F, aspect,
            std::max(0.001F, camera_.pose().distance * 0.001F),
            std::max(100.0F, camera_.pose().distance + camera_.sceneRadius() * 8.0F));
        return const_cast<StackVulkanWindow*>(this)->clipCorrectionMatrix() *
               projection * camera_.viewMatrix();
    }
    void updateSceneBounds() {
        MaskStackPreviewDataPtr preview;
        MaskStackDisplayMode mode;
        float zScale;
        {
            QMutexLocker lock(&mutex_);
            preview = preview_;
            mode = mode_;
            zScale = zScale_;
        }
        if (!preview) {
            { QMutexLocker lock(&mutex_); origin_ = {}; }
            camera_.setSceneBounds({-0.5F,-0.5F,-0.5F},{0.5F,0.5F,0.5F});
            return;
        }
        const float width = static_cast<float>(preview->physicalSize.width());
        const float height = static_cast<float>(preview->physicalSize.height());
        const float explode = mode == MaskStackDisplayMode::Exploded ? kExplodedSpacing : 1.0F;
        const float stackHeight = static_cast<float>(preview->layerCount * preview->layerHeight) *
                                  zScale * explode;
        const QVector3D origin(width * 0.5F, height * 0.5F, stackHeight * 0.5F);
        { QMutexLocker lock(&mutex_); origin_ = origin; }
        camera_.setSceneBounds(-origin, {width-origin.x(), height-origin.y(),
                                         stackHeight-origin.z()});
    }
    void addLine(float x0,float y0,float z0,float x1,float y1,float z1,
                 float r,float g,float b) {
        lines_.push_back({x0,y0,z0,r,g,b});
        lines_.push_back({x1,y1,z1,r,g,b});
    }
    quint16 levelAtLocked(int x, int y) const {
        if (!preview_ || x < 0 || y < 0 || x >= preview_->gridSize.width() ||
            y >= preview_->gridSize.height()) return 0;
        return preview_->columnLayers[y * preview_->gridSize.width() + x];
    }
    void rebuildLinesLocked() {
        lines_.clear();
        linesDirty_ = true;
        if (!preview_) return;
        const float width = static_cast<float>(preview_->physicalSize.width());
        const float height = static_cast<float>(preview_->physicalSize.height());
        const float stackHeight = static_cast<float>(preview_->layerCount * preview_->layerHeight);
        constexpr float grid = 0.32F;
        addLine(0,0,0,width,0,0,1.0F,0.25F,0.25F);
        addLine(0,0,0,0,height,0,0.25F,1.0F,0.35F);
        addLine(0,0,0,0,0,stackHeight,0.25F,0.55F,1.0F);
        addLine(width,0,0,width,height,0,grid,grid,grid);
        addLine(width,height,0,0,height,0,grid,grid,grid);
        addLine(0,height,0,0,0,0,grid,grid,grid);

        const auto active = std::find_if(preview_->blocks.cbegin(), preview_->blocks.cend(),
            [this](const MaskStackBlockPlacement& block) {
                return block.traversalIndex == activeBlock_;
            });
        if (active != preview_->blocks.cend()) {
            const float x0 = static_cast<float>(active->physicalRect.left());
            const float x1 = static_cast<float>(active->physicalRect.right());
            const float y0 = static_cast<float>(active->physicalRect.top());
            const float y1 = static_cast<float>(active->physicalRect.bottom());
            constexpr float r = 1.0F, g = 0.82F, b = 0.05F;
            addLine(x0,y0,0,x1,y0,0,r,g,b); addLine(x1,y0,0,x1,y1,0,r,g,b);
            addLine(x1,y1,0,x0,y1,0,r,g,b); addLine(x0,y1,0,x0,y0,0,r,g,b);
            addLine(x0,y0,0,x0,y0,stackHeight,r,g,b);
            addLine(x1,y0,0,x1,y0,stackHeight,r,g,b);
            addLine(x1,y1,0,x1,y1,stackHeight,r,g,b);
            addLine(x0,y1,0,x0,y1,stackHeight,r,g,b);
        }

        if (selectedLayer_ < 1 || selectedLayer_ > preview_->layerCount) return;
        const int gridWidth = preview_->gridSize.width();
        const int gridHeight = preview_->gridSize.height();
        const float pitchX = width / std::max(1, gridWidth);
        const float pitchY = height / std::max(1, gridHeight);
        const float z = static_cast<float>(selectedLayer_ * preview_->layerHeight);
        for (int y = 0; y < gridHeight; ++y) {
            for (int x = 0; x < gridWidth; ++x) {
                if (levelAtLocked(x,y) < selectedLayer_) continue;
                const float x0=x*pitchX,x1=(x+1)*pitchX;
                const float y0=(gridHeight-y-1)*pitchY,y1=(gridHeight-y)*pitchY;
                if (levelAtLocked(x-1,y)<selectedLayer_) addLine(x0,y0,z,x0,y1,z,1,0.82F,0.05F);
                if (levelAtLocked(x+1,y)<selectedLayer_) addLine(x1,y1,z,x1,y0,z,1,0.82F,0.05F);
                if (levelAtLocked(x,y-1)<selectedLayer_) addLine(x0,y1,z,x1,y1,z,1,0.82F,0.05F);
                if (levelAtLocked(x,y+1)<selectedLayer_) addLine(x1,y0,z,x0,y0,z,1,0.82F,0.05F);
            }
        }
    }

    StackVulkanRenderer* renderer_ = nullptr;
    mutable QMutex mutex_;
    MaskStackPreviewDataPtr preview_;
    MaskStackDisplayMode mode_ = MaskStackDisplayMode::Solid;
    QColor startColor_{0,0,255};
    QColor endColor_{0,255,0};
    int selectedLayer_ = 1;
    qint64 activeBlock_ = -1;
    float zScale_ = 1.0F;
    QVector3D origin_;
    JobTerrainCamera camera_;
    std::vector<StackLineVertex> lines_;
    bool geometryDirty_ = true;
    bool linesDirty_ = true;
    Qt::MouseButton pressedButton_ = Qt::NoButton;
    QPointF lastPosition_;
};

bool StackVulkanRenderer::createBuffer(VkDeviceSize size,VkBufferUsageFlags usage,Buffer* buffer) {
    if(!buffer||size==0)return false;VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size=size;info.usage=usage;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(functions_->vkCreateBuffer(device_,&info,nullptr,&buffer->buffer)!=VK_SUCCESS)return false;
    VkMemoryRequirements requirements{};functions_->vkGetBufferMemoryRequirements(device_,buffer->buffer,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=window_->hostVisibleMemoryIndex();
    if(functions_->vkAllocateMemory(device_,&allocation,nullptr,&buffer->memory)!=VK_SUCCESS){functions_->vkDestroyBuffer(device_,buffer->buffer,nullptr);buffer->buffer=VK_NULL_HANDLE;return false;}
    if(functions_->vkBindBufferMemory(device_,buffer->buffer,buffer->memory,0)!=VK_SUCCESS){destroyBuffer(buffer);return false;}
    buffer->capacity=size;return true;
}
void StackVulkanRenderer::destroyBuffer(Buffer* buffer){if(!buffer||!functions_)return;if(buffer->buffer)functions_->vkDestroyBuffer(device_,buffer->buffer,nullptr);if(buffer->memory)functions_->vkFreeMemory(device_,buffer->memory,nullptr);*buffer={};}
bool StackVulkanRenderer::upload(Buffer* buffer,const void* data,VkDeviceSize size,VkBufferUsageFlags usage){if(!buffer||!data||!size)return false;if(buffer->capacity<size){destroyBuffer(buffer);if(!createBuffer(std::max<VkDeviceSize>(size,size*3/2),usage,buffer))return false;}void* mapped=nullptr;if(functions_->vkMapMemory(device_,buffer->memory,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS)return false;std::memcpy(mapped,data,static_cast<size_t>(size));VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=buffer->memory;range.size=VK_WHOLE_SIZE;functions_->vkFlushMappedMemoryRanges(device_,1,&range);functions_->vkUnmapMemory(device_,buffer->memory);return true;}
VkShaderModule StackVulkanRenderer::loadShader(const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return VK_NULL_HANDLE;const QByteArray code=file.readAll();if(code.isEmpty()||code.size()%4)return VK_NULL_HANDLE;VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=static_cast<size_t>(code.size());info.pCode=reinterpret_cast<const uint32_t*>(code.constData());VkShaderModule module=VK_NULL_HANDLE;return functions_->vkCreateShaderModule(device_,&info,nullptr,&module)==VK_SUCCESS?module:VK_NULL_HANDLE;}
VkPipeline StackVulkanRenderer::createPipeline(VkShaderModule vertex,VkShaderModule fragment,bool lines){VkPipelineShaderStageCreateInfo stages[2]{};stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[0].pName="main";stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;stages[1].pName="main";VkVertexInputBindingDescription binding{};binding.binding=0;binding.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;binding.stride=lines?sizeof(StackLineVertex):sizeof(MaskStackVertex);VkVertexInputAttributeDescription attrs[4]{};if(lines){attrs[0]={0,0,VK_FORMAT_R32G32B32_SFLOAT,0};attrs[1]={1,0,VK_FORMAT_R32G32B32_SFLOAT,12};}else{attrs[0]={0,0,VK_FORMAT_R32G32B32_SFLOAT,0};attrs[1]={1,0,VK_FORMAT_R32G32B32_SFLOAT,12};attrs[2]={2,0,VK_FORMAT_R32_SFLOAT,24};attrs[3]={3,0,VK_FORMAT_R32_SFLOAT,28};}VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};input.vertexBindingDescriptionCount=1;input.pVertexBindingDescriptions=&binding;input.vertexAttributeDescriptionCount=lines?2U:4U;input.pVertexAttributeDescriptions=attrs;VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=lines?VK_PRIMITIVE_TOPOLOGY_LINE_LIST:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=1;viewport.scissorCount=1;VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1.0F;VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=window_->sampleCountFlagBits();VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depth.depthTestEnable=lines?VK_FALSE:VK_TRUE;depth.depthWriteEnable=lines?VK_FALSE:VK_TRUE;depth.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=0xF;VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;const VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.stageCount=2;info.pStages=stages;info.pVertexInputState=&input;info.pInputAssemblyState=&assembly;info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&multisample;info.pDepthStencilState=&depth;info.pColorBlendState=&blend;info.pDynamicState=&dynamic;info.layout=pipelineLayout_;info.renderPass=window_->defaultRenderPass();VkPipeline pipeline=VK_NULL_HANDLE;return functions_->vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&info,nullptr,&pipeline)==VK_SUCCESS?pipeline:VK_NULL_HANDLE;}
void StackVulkanRenderer::initResources(){device_=window_->device();functions_=window_->vulkanInstance()->deviceFunctions(device_);VkDescriptorSetLayoutBinding binding{};binding.binding=0;binding.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=1;layout.pBindings=&binding;if(functions_->vkCreateDescriptorSetLayout(device_,&layout,nullptr,&descriptorLayout_)!=VK_SUCCESS)return;VkPipelineLayoutCreateInfo pipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipeline.setLayoutCount=1;pipeline.pSetLayouts=&descriptorLayout_;if(functions_->vkCreatePipelineLayout(device_,&pipeline,nullptr,&pipelineLayout_)!=VK_SUCCESS)return;const int frames=window_->concurrentFrameCount();uniformBuffers_.resize(frames);descriptorSets_.resize(frames);VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,static_cast<uint32_t>(frames)};VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=frames;pool.poolSizeCount=1;pool.pPoolSizes=&poolSize;if(functions_->vkCreateDescriptorPool(device_,&pool,nullptr,&descriptorPool_)!=VK_SUCCESS)return;std::vector<VkDescriptorSetLayout> layouts(frames,descriptorLayout_);VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=descriptorPool_;allocate.descriptorSetCount=frames;allocate.pSetLayouts=layouts.data();if(functions_->vkAllocateDescriptorSets(device_,&allocate,descriptorSets_.data())!=VK_SUCCESS)return;for(int i=0;i<frames;++i){if(!createBuffer(sizeof(StackUniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,&uniformBuffers_[i]))return;VkDescriptorBufferInfo info{uniformBuffers_[i].buffer,0,sizeof(StackUniform)};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=descriptorSets_[i];write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&info;functions_->vkUpdateDescriptorSets(device_,1,&write,0,nullptr);}VkShaderModule sv=loadShader(QStringLiteral(":/mask_stack_shaders/mask_stack.vert.spv"));VkShaderModule sf=loadShader(QStringLiteral(":/mask_stack_shaders/mask_stack.frag.spv"));VkShaderModule lv=loadShader(QStringLiteral(":/mask_stack_shaders/mask_stack_lines.vert.spv"));VkShaderModule lf=loadShader(QStringLiteral(":/mask_stack_shaders/mask_stack_lines.frag.spv"));if(sv&&sf)surfacePipeline_=createPipeline(sv,sf,false);if(lv&&lf)linePipeline_=createPipeline(lv,lf,true);for(VkShaderModule module:{sv,sf,lv,lf})if(module)functions_->vkDestroyShaderModule(device_,module,nullptr);initialized_=surfacePipeline_&&linePipeline_;rebuildGeometry();rebuildLines();}
void StackVulkanRenderer::rebuildGeometry(){std::shared_ptr<const MaskStackGeometry> geometry;if(!window_->takeGeometry(&geometry))return;indexCount_=0;if(!geometry||geometry->vertices.empty()||geometry->indices.empty())return;if(upload(&vertexBuffer_,geometry->vertices.data(),geometry->vertices.size()*sizeof(MaskStackVertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)&&upload(&indexBuffer_,geometry->indices.data(),geometry->indices.size()*sizeof(std::uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT))indexCount_=static_cast<std::uint32_t>(std::min<std::size_t>(geometry->indices.size(),std::numeric_limits<std::uint32_t>::max()));}
void StackVulkanRenderer::rebuildLines(){std::vector<StackLineVertex> lines;if(!window_->takeLines(&lines))return;lineCount_=0;if(lines.empty())return;if(upload(&lineBuffer_,lines.data(),lines.size()*sizeof(StackLineVertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))lineCount_=static_cast<std::uint32_t>(std::min<std::size_t>(lines.size(),std::numeric_limits<std::uint32_t>::max()));}
void StackVulkanRenderer::startNextFrame(){if(!initialized_){window_->frameReady();return;}rebuildGeometry();rebuildLines();const int frame=window_->currentFrame();const StackUniform uniform=window_->uniform();upload(&uniformBuffers_[frame],&uniform,sizeof(uniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);VkClearValue clears[2]{};clears[0].color={{0.025F,0.035F,0.065F,1.0F}};clears[1].depthStencil={1.0F,0};VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=window_->defaultRenderPass();begin.framebuffer=window_->currentFramebuffer();begin.renderArea.extent={static_cast<uint32_t>(window_->swapChainImageSize().width()),static_cast<uint32_t>(window_->swapChainImageSize().height())};begin.clearValueCount=2;begin.pClearValues=clears;VkCommandBuffer command=window_->currentCommandBuffer();functions_->vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);VkViewport viewport{0,0,float(begin.renderArea.extent.width),float(begin.renderArea.extent.height),0,1};VkRect2D scissor{{0,0},begin.renderArea.extent};functions_->vkCmdSetViewport(command,0,1,&viewport);functions_->vkCmdSetScissor(command,0,1,&scissor);functions_->vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout_,0,1,&descriptorSets_[frame],0,nullptr);const VkDeviceSize offset=0;if(indexCount_&&vertexBuffer_.buffer&&indexBuffer_.buffer){functions_->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,surfacePipeline_);functions_->vkCmdBindVertexBuffers(command,0,1,&vertexBuffer_.buffer,&offset);functions_->vkCmdBindIndexBuffer(command,indexBuffer_.buffer,0,VK_INDEX_TYPE_UINT32);functions_->vkCmdDrawIndexed(command,indexCount_,1,0,0,0);}if(lineCount_&&lineBuffer_.buffer){functions_->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,linePipeline_);functions_->vkCmdBindVertexBuffers(command,0,1,&lineBuffer_.buffer,&offset);functions_->vkCmdDraw(command,lineCount_,1,0,0);}functions_->vkCmdEndRenderPass(command);window_->frameReady();}
void StackVulkanRenderer::releaseResources(){if(!functions_)return;for(auto& buffer:uniformBuffers_)destroyBuffer(&buffer);destroyBuffer(&vertexBuffer_);destroyBuffer(&indexBuffer_);destroyBuffer(&lineBuffer_);if(surfacePipeline_)functions_->vkDestroyPipeline(device_,surfacePipeline_,nullptr);if(linePipeline_)functions_->vkDestroyPipeline(device_,linePipeline_,nullptr);if(pipelineLayout_)functions_->vkDestroyPipelineLayout(device_,pipelineLayout_,nullptr);if(descriptorPool_)functions_->vkDestroyDescriptorPool(device_,descriptorPool_,nullptr);if(descriptorLayout_)functions_->vkDestroyDescriptorSetLayout(device_,descriptorLayout_,nullptr);initialized_=false;functions_=nullptr;device_=VK_NULL_HANDLE;}
} // namespace
#endif

class MaskStackWidget::Backend {
public:
    explicit Backend(QWidget* host) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        instance_ = std::make_unique<QVulkanInstance>();
        if (!instance_->create()) {
            error_ = QStringLiteral("Vulkan 初始化失败（错误码 %1）。").arg(instance_->errorCode());
            return;
        }
        window_ = new StackVulkanWindow;
        window_->setObjectName(QStringLiteral("maskStackVulkanWindow"));
        window_->setVulkanInstance(instance_.get());
        if (window_->availablePhysicalDevices().isEmpty()) {
            delete window_; window_ = nullptr;
            error_ = QStringLiteral("未找到可用于堆叠预览的 Vulkan 图形设备。");
            return;
        }
        container_ = QWidget::createWindowContainer(window_, host);
        container_->setFocusPolicy(Qt::StrongFocus);
#else
        Q_UNUSED(host);
        error_ = QStringLiteral("此构建未启用 Vulkan 堆叠预览。请安装 Vulkan SDK 和 Qt ShaderTools 后重新配置。");
#endif
    }
    ~Backend() {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        delete container_; container_ = nullptr; window_ = nullptr;
#endif
    }
    bool available() const {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        return window_ != nullptr;
#else
        return false;
#endif
    }
    QWidget* widget() const {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        return container_;
#else
        return nullptr;
#endif
    }
    QString error() const { return error_; }
    void setPreview(const MaskStackPreviewDataPtr& preview) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->setPreview(preview);
#else
        Q_UNUSED(preview);
#endif
    }
    void setColors(const QColor& start,const QColor& end) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->setColors(start,end);
#else
        Q_UNUSED(start);Q_UNUSED(end);
#endif
    }
    void setMode(MaskStackDisplayMode mode) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->setMode(mode);
#else
        Q_UNUSED(mode);
#endif
    }
    void setSelectedLayer(int layer) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->setSelectedLayer(layer);
#else
        Q_UNUSED(layer);
#endif
    }
    void setActiveBlock(qint64 traversalIndex) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->setActiveBlock(traversalIndex);
#else
        Q_UNUSED(traversalIndex);
#endif
    }
    void setZScale(double scale) {
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->setZScale(scale);
#else
        Q_UNUSED(scale);
#endif
    }
    void fitAll(){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->fitAll();
#endif
    }
    void top(){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->viewTop();
#endif
    }
    void front(){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->viewFront();
#endif
    }
    void right(){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->viewRight();
#endif
    }
    QImage grab(){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        return window_?window_->grab():QImage();
#else
        return {};
#endif
    }
private:
    QString error_;
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
    std::unique_ptr<QVulkanInstance> instance_;
    StackVulkanWindow* window_ = nullptr;
    QWidget* container_ = nullptr;
#endif
};

MaskStackWidget::MaskStackWidget(QWidget* parent) : QWidget(parent), backend_(new Backend(this)) {
    setObjectName(QStringLiteral("maskStackWidget"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    if (backend_->available()) layout->addWidget(backend_->widget(), 1);
    else {
        auto* error = new QLabel(backend_->error());
        error->setObjectName(QStringLiteral("maskStackVulkanError"));
        error->setAlignment(Qt::AlignCenter);
        error->setWordWrap(true);
        error->setStyleSheet(QStringLiteral("QLabel { color: #ff8a80; background: #25282b; padding: 24px; }"));
        layout->addWidget(error, 1);
    }
    auto* shortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    shortcut->setObjectName(QStringLiteral("maskStackResetShortcut"));
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, &MaskStackWidget::fitAll);
}
MaskStackWidget::~MaskStackWidget(){delete backend_;}
bool MaskStackWidget::isVulkanAvailable()const noexcept{return backend_&&backend_->available();}
void MaskStackWidget::setPreview(const MaskStackPreviewDataPtr& preview){preview_=preview;backend_->setPreview(preview_);}
void MaskStackWidget::setColors(const QColor& start,const QColor& end){startColor_=start;endColor_=end;backend_->setColors(start,end);}
void MaskStackWidget::setDisplayMode(MaskStackDisplayMode mode){mode_=mode;backend_->setMode(mode);}
void MaskStackWidget::setSelectedLayer(int layer){selectedLayer_=std::max(1,layer);backend_->setSelectedLayer(selectedLayer_);}
void MaskStackWidget::setActiveBlock(qint64 traversalIndex){activeBlock_=traversalIndex;backend_->setActiveBlock(traversalIndex);}
void MaskStackWidget::setZScale(double scale){zScale_=std::clamp(scale,1.0,100.0);backend_->setZScale(zScale_);}
void MaskStackWidget::clear(){preview_.reset();activeBlock_=-1;backend_->setPreview({});backend_->setActiveBlock(-1);}
void MaskStackWidget::fitAll(){backend_->fitAll();}
void MaskStackWidget::viewTop(){backend_->top();}
void MaskStackWidget::viewFront(){backend_->front();}
void MaskStackWidget::viewRight(){backend_->right();}
QImage MaskStackWidget::grabFrame(){return backend_->grab();}

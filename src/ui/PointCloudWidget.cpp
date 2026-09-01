#include "ui/PointCloudWidget.h"

#include "imaging/JobPreviewMath.h"
#include "imaging/PointCloudBuilder.h"
#include "imaging/PointCloudMath.h"

#include <QCheckBox>
#include <QFile>
#include <QFocusEvent>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QShortcut>
#include <QStyleHints>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
#include <QMatrix4x4>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QVulkanWindow>

namespace {
constexpr float kPi = 3.14159265358979323846F;

struct CloudLineVertex { float x, y, z, r, g, b, overlay; };
struct alignas(16) CloudCameraUniform {
    float mvp[16];
    float pixelScale = 1.0F;
    float maximumHeight = 1.0F;
    float originX = 0.0F;
    float originY = 0.0F;
    float originZ = 0.0F;
    float invertHeight = 1.0F;
    float viewportSize[2]{1.0F, 1.0F};
    float startColor[4]{0.0F, 0.0F, 1.0F, 1.0F};
    float endColor[4]{0.0F, 1.0F, 0.0F, 1.0F};
};
static_assert(offsetof(CloudCameraUniform, viewportSize) == 88);
static_assert(offsetof(CloudCameraUniform, startColor) == 96);
static_assert(sizeof(CloudCameraUniform) == 128);
static_assert(sizeof(PointCloudVertex) == 12);

class CloudVulkanWindow;

class CloudVulkanRenderer final : public QVulkanWindowRenderer {
public:
    explicit CloudVulkanRenderer(CloudVulkanWindow* window) : window_(window) {}
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
    bool upload(Buffer* buffer, const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    VkShaderModule loadShader(const QString& path);
    VkPipeline createPipeline(VkShaderModule vertex, VkShaderModule fragment, bool points);
    void rebuildPointBuffer();
    void rebuildLineBuffer();

    CloudVulkanWindow* window_ = nullptr;
    QVulkanDeviceFunctions* functions_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipeline pointPipeline_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;
    Buffer pointBuffer_;
    Buffer lineBuffer_;
    std::vector<Buffer> uniformBuffers_;
    std::vector<VkDescriptorSet> descriptorSets_;
    uint32_t pointCount_ = 0;
    uint32_t lineCount_ = 0;
    bool initialized_ = false;
};

class CloudVulkanWindow final : public QVulkanWindow {
public:
    CloudVulkanWindow() { camera_.setElevationLimits(-89.0F, 89.0F); }
    QVulkanWindowRenderer* createRenderer() override {
        renderer_ = new CloudVulkanRenderer(this);
        return renderer_;
    }

    void setDocument(const PointCloudDocumentPtr& document) {
        document_ = document;
        if (document_ && document_->source) {
            const float width = static_cast<float>(std::max<qint64>(0, document_->source->metadata.width - 1)) *
                                static_cast<float>(document_->settings.pixelScale);
            const float height = static_cast<float>(std::max<qint64>(0, document_->source->metadata.height - 1)) *
                                 static_cast<float>(document_->settings.pixelScale);
            origin_ = {width * 0.5F, height * 0.5F,
                       static_cast<float>(document_->settings.maximumHeight) * 0.5F};
            camera_.setSceneBounds(-origin_, {width - origin_.x(), height - origin_.y(),
                                              static_cast<float>(document_->settings.maximumHeight) - origin_.z()});
        } else {
            origin_ = {};
            camera_.setSceneBounds({-0.5F,-0.5F,-0.5F}, {0.5F,0.5F,0.5F});
        }
        linesDirty_ = true;
        notifyViewChanged();
        requestUpdate();
    }

    void setVertices(std::vector<PointCloudVertex> vertices) {
        QMutexLocker lock(&vertexMutex_);
        vertices_ = std::move(vertices);
        verticesDirty_ = true;
        requestUpdate();
    }

    bool takeVertices(std::vector<PointCloudVertex>* vertices) {
        QMutexLocker lock(&vertexMutex_);
        if (!verticesDirty_) return false;
        *vertices = vertices_;
        verticesDirty_ = false;
        return true;
    }

    bool takeLinesDirty() { return std::exchange(linesDirty_, false); }
    void setGuidesVisible(bool visible) { guidesVisible_ = visible; linesDirty_ = true; requestUpdate(); }
    void fitAll() { clearPointerInteraction(); camera_.setPose(camera_.fittedPose()); notifyViewChanged(); requestUpdate(); }
    const PointCloudDocumentPtr& document() const { return document_; }
    QVector3D origin() const { return origin_; }

    QMatrix4x4 viewProjection() const {
        const QSize renderSize = swapChainImageSize().isValid() ? swapChainImageSize() : size();
        const float aspect = renderSize.height() > 0 ? float(renderSize.width()) / renderSize.height() : 1.0F;
        QMatrix4x4 projection;
        projection.perspective(45.0F, aspect,
            std::max(0.001F, camera_.pose().distance * 0.001F),
            std::max(100.0F, camera_.pose().distance + camera_.sceneRadius() * 8.0F));
        return const_cast<CloudVulkanWindow*>(this)->clipCorrectionMatrix() * projection * camera_.viewMatrix();
    }

    std::vector<CloudLineVertex> lineVertices() const {
        std::vector<CloudLineVertex> result;
        if (!document_ || !document_->source || !guidesVisible_) return result;
        const float x0 = -origin_.x(), x1 = origin_.x();
        const float y0 = -origin_.y(), y1 = origin_.y();
        const float z0 = -origin_.z(), z1 = static_cast<float>(document_->settings.maximumHeight) - origin_.z();
        auto add = [&result](QVector3D a, QVector3D b, QVector3D color) {
            result.push_back({a.x(),a.y(),a.z(),color.x(),color.y(),color.z(),0.0F});
            result.push_back({b.x(),b.y(),b.z(),color.x(),color.y(),color.z(),0.0F});
        };
        const QVector3D grid(0.20F,0.23F,0.27F);
        for (int i = 0; i <= 10; ++i) {
            const float t = i / 10.0F;
            add({x0, y0 + (y1-y0)*t, z0}, {x1, y0 + (y1-y0)*t, z0}, grid);
            add({x0 + (x1-x0)*t, y0, z0}, {x0 + (x1-x0)*t, y1, z0}, grid);
        }
        add({x0,y0,z0},{x1,y0,z0},{1.0F,0.25F,0.25F});
        add({x0,y0,z0},{x0,y1,z0},{0.25F,1.0F,0.35F});
        add({x0,y0,z0},{x0,y0,z1},{0.25F,0.55F,1.0F});
        const QMatrix4x4 mvp=viewProjection();QPointF xScreen,yScreen;
        if(JobPreviewMath::project({x1,y0,z0},mvp,QSizeF(width(),height()),&xScreen))appendOutlinedText(result,xScreen+QPointF(6,-14),QStringLiteral("X MAX %1").arg(PointCloudMath::formatAxisMaximum(PointCloudMath::axisMaximum(document_->source->metadata.width,document_->settings.pixelScale))),{1.0F,0.25F,0.25F});
        if(JobPreviewMath::project({x0,y1,z0},mvp,QSizeF(width(),height()),&yScreen))appendOutlinedText(result,yScreen+QPointF(6,-14),QStringLiteral("Y MAX %1").arg(PointCloudMath::formatAxisMaximum(PointCloudMath::axisMaximum(document_->source->metadata.height,document_->settings.pixelScale))),{0.25F,1.0F,0.35F});
        return result;
    }

    std::function<void(float,float,float,float,float)> viewChanged;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button()!=Qt::LeftButton && event->button()!=Qt::MiddleButton && event->button()!=Qt::RightButton) return;
        pressedButton_=event->button(); lastPosition_=event->position(); requestActivate();
        setMouseGrabEnabled(true); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (pressedButton_ == Qt::NoButton) return;
        if (!event->buttons().testFlag(pressedButton_)) { clearPointerInteraction(); return; }
        const QPointF delta=event->position()-lastPosition_;
        if (pressedButton_==Qt::LeftButton) camera_.orbit(delta);
        else if (pressedButton_==Qt::MiddleButton) {
            const float scale=worldPerPixel();
            camera_.translate(camera_.right()*float(-delta.x())*scale +
                              camera_.groundForward()*float(delta.y())*scale);
        } else camera_.zoomBy(std::exp(float(delta.y())*0.01F));
        lastPosition_=event->position(); notifyViewChanged(); requestUpdate(); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if(event->button()!=pressedButton_)return; clearPointerInteraction();event->accept();
    }
    void focusOutEvent(QFocusEvent* event) override {
        clearPointerInteraction();
        QVulkanWindow::focusOutEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Space) {
            fitAll();
            event->accept();
            return;
        }
        QVulkanWindow::keyPressEvent(event);
    }
    void wheelEvent(QWheelEvent* event) override {
        camera_.zoomBy(std::exp(-float(event->angleDelta().y())*0.0012F));
        notifyViewChanged(); requestUpdate(); event->accept();
    }
    void resizeEvent(QResizeEvent* event) override {
        QVulkanWindow::resizeEvent(event); notifyViewChanged();
    }

private:
    friend class CloudVulkanRenderer;
    float worldPerPixel() const {
        return std::max(0.000001F, 2.0F*camera_.pose().distance*std::tan(22.5F*kPi/180.0F)/std::max(1,height()));
    }
    void notifyViewChanged() {
        linesDirty_ = true;
        if (!viewChanged) return;
        const float scale=worldPerPixel();
        viewChanged(camera_.pose().pivot.x(), camera_.pose().pivot.y(), scale,
                    scale*std::max(1,width()), scale*std::max(1,height()));
    }
    void clearPointerInteraction() {
        const bool hadPointerInteraction = pressedButton_ != Qt::NoButton;
        pressedButton_ = Qt::NoButton;
        if (hadPointerInteraction) setMouseGrabEnabled(false);
    }

    QVector2D pixelToNdc(const QPointF& point) const{return JobPreviewMath::screenToVulkanNdc(point,QSizeF(width(),height()));}
    void appendOverlayLine(std::vector<CloudLineVertex>& lines,const QPointF& a,const QPointF& b,const QVector3D& color)const{const QVector2D na=pixelToNdc(a),nb=pixelToNdc(b);lines.push_back({na.x(),na.y(),0.0F,color.x(),color.y(),color.z(),1.0F});lines.push_back({nb.x(),nb.y(),0.0F,color.x(),color.y(),color.z(),1.0F});}
    void appendGlyph(std::vector<CloudLineVertex>& lines,float x,float y,QChar glyph,const QVector3D& color)const{static constexpr unsigned char masks[10]={0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};const float w=6.0F,h=10.0F;const QPointF points[6]={{x,y},{x+w,y},{x,y+h*0.5F},{x+w,y+h*0.5F},{x,y+h},{x+w,y+h}};const int pairs[7][2]={{0,1},{1,3},{3,5},{4,5},{2,4},{0,2},{2,3}};unsigned char mask=0;if(glyph.isDigit())mask=masks[glyph.digitValue()];else if(glyph==QLatin1Char('-'))mask=0x40;if(mask){for(int segment=0;segment<7;++segment)if(mask&(1U<<segment))appendOverlayLine(lines,points[pairs[segment][0]],points[pairs[segment][1]],color);}else if(glyph==QLatin1Char('.'))appendOverlayLine(lines,{x+2,y+h},{x+3,y+h},color);else if(glyph==QLatin1Char('X')){appendOverlayLine(lines,{x,y},{x+w,y+h},color);appendOverlayLine(lines,{x+w,y},{x,y+h},color);}else if(glyph==QLatin1Char('Y')){appendOverlayLine(lines,{x,y},{x+w*0.5F,y+h*0.5F},color);appendOverlayLine(lines,{x+w,y},{x+w*0.5F,y+h*0.5F},color);appendOverlayLine(lines,{x+w*0.5F,y+h*0.5F},{x+w*0.5F,y+h},color);}else if(glyph==QLatin1Char('M')){appendOverlayLine(lines,{x,y+h},{x,y},color);appendOverlayLine(lines,{x,y},{x+w*0.5F,y+h*0.5F},color);appendOverlayLine(lines,{x+w*0.5F,y+h*0.5F},{x+w,y},color);appendOverlayLine(lines,{x+w,y},{x+w,y+h},color);}else if(glyph==QLatin1Char('A')){appendOverlayLine(lines,{x,y+h},{x+w*0.5F,y},color);appendOverlayLine(lines,{x+w*0.5F,y},{x+w,y+h},color);appendOverlayLine(lines,{x+1,y+h*0.55F},{x+w-1,y+h*0.55F},color);}}
    void appendText(std::vector<CloudLineVertex>& lines,const QPointF& anchor,const QString& text,const QVector3D& color)const{float x=float(anchor.x());const float y=float(anchor.y());for(QChar glyph:text){appendGlyph(lines,x,y,glyph,color);x+=8.0F;}}
    void appendOutlinedText(std::vector<CloudLineVertex>& lines,const QPointF& anchor,const QString& text,const QVector3D& color)const{const QVector3D outline(0.96F,0.96F,0.96F);static constexpr QPointF offsets[]={{-1.5,0},{1.5,0},{0,-1.5},{0,1.5}};for(const QPointF& offset:offsets)appendText(lines,anchor+offset,text,outline);appendText(lines,anchor,text,color);}

    CloudVulkanRenderer* renderer_ = nullptr;
    PointCloudDocumentPtr document_;
    JobTerrainCamera camera_;
    QVector3D origin_;
    QMutex vertexMutex_;
    std::vector<PointCloudVertex> vertices_;
    bool verticesDirty_ = true;
    bool linesDirty_ = true;
    bool guidesVisible_ = true;
    Qt::MouseButton pressedButton_ = Qt::NoButton;
    QPointF lastPosition_;
};

bool CloudVulkanRenderer::createBuffer(VkDeviceSize size,VkBufferUsageFlags usage,Buffer* buffer){
    if(!buffer||size==0)return false;VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size=size;info.usage=usage;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(functions_->vkCreateBuffer(device_,&info,nullptr,&buffer->buffer)!=VK_SUCCESS)return false;
    VkMemoryRequirements requirements{};functions_->vkGetBufferMemoryRequirements(device_,buffer->buffer,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=window_->hostVisibleMemoryIndex();
    if(functions_->vkAllocateMemory(device_,&allocation,nullptr,&buffer->memory)!=VK_SUCCESS){functions_->vkDestroyBuffer(device_,buffer->buffer,nullptr);buffer->buffer=VK_NULL_HANDLE;return false;}
    if(functions_->vkBindBufferMemory(device_,buffer->buffer,buffer->memory,0)!=VK_SUCCESS){destroyBuffer(buffer);return false;}
    buffer->capacity=size;return true;
}
void CloudVulkanRenderer::destroyBuffer(Buffer* buffer){if(!buffer||!functions_)return;if(buffer->buffer)functions_->vkDestroyBuffer(device_,buffer->buffer,nullptr);if(buffer->memory)functions_->vkFreeMemory(device_,buffer->memory,nullptr);*buffer={};}
bool CloudVulkanRenderer::upload(Buffer* buffer,const void* data,VkDeviceSize size,VkBufferUsageFlags usage){
    if(!buffer||!data||!size)return false;if(buffer->capacity<size){destroyBuffer(buffer);if(!createBuffer(std::max<VkDeviceSize>(size,size*3/2),usage,buffer))return false;}
    void* mapped=nullptr;if(functions_->vkMapMemory(device_,buffer->memory,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS)return false;std::memcpy(mapped,data,static_cast<size_t>(size));
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=buffer->memory;range.size=VK_WHOLE_SIZE;functions_->vkFlushMappedMemoryRanges(device_,1,&range);functions_->vkUnmapMemory(device_,buffer->memory);return true;
}
VkShaderModule CloudVulkanRenderer::loadShader(const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return VK_NULL_HANDLE;const QByteArray code=file.readAll();if(code.isEmpty()||code.size()%4)return VK_NULL_HANDLE;VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=static_cast<size_t>(code.size());info.pCode=reinterpret_cast<const uint32_t*>(code.constData());VkShaderModule module=VK_NULL_HANDLE;return functions_->vkCreateShaderModule(device_,&info,nullptr,&module)==VK_SUCCESS?module:VK_NULL_HANDLE;}
VkPipeline CloudVulkanRenderer::createPipeline(VkShaderModule vertex,VkShaderModule fragment,bool points){
    VkPipelineShaderStageCreateInfo stages[2]{};stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[0].pName="main";stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;stages[1].pName="main";
    VkVertexInputBindingDescription binding{};binding.binding=0;binding.inputRate=points?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX;binding.stride=points?sizeof(PointCloudVertex):sizeof(CloudLineVertex);
    VkVertexInputAttributeDescription attrs[3]{};attrs[0]={0,0,points?VK_FORMAT_R32G32_SFLOAT:VK_FORMAT_R32G32B32_SFLOAT,0};attrs[1]={1,0,points?VK_FORMAT_R32_SFLOAT:VK_FORMAT_R32G32B32_SFLOAT,points?8U:12U};attrs[2]={2,0,VK_FORMAT_R32_SFLOAT,24U};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};input.vertexBindingDescriptionCount=1;input.pVertexBindingDescriptions=&binding;input.vertexAttributeDescriptionCount=points?2U:3U;input.pVertexAttributeDescriptions=attrs;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=points?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=1;viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=window_->sampleCountFlagBits();
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depth.depthTestEnable=points;depth.depthWriteEnable=points;depth.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=0xF;attachment.blendEnable=VK_TRUE;attachment.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;attachment.colorBlendOp=VK_BLEND_OP_ADD;attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;attachment.alphaBlendOp=VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;const VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.stageCount=2;info.pStages=stages;info.pVertexInputState=&input;info.pInputAssemblyState=&assembly;info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&multisample;info.pDepthStencilState=&depth;info.pColorBlendState=&blend;info.pDynamicState=&dynamic;info.layout=pipelineLayout_;info.renderPass=window_->defaultRenderPass();VkPipeline pipeline=VK_NULL_HANDLE;return functions_->vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&info,nullptr,&pipeline)==VK_SUCCESS?pipeline:VK_NULL_HANDLE;
}
void CloudVulkanRenderer::initResources(){
    device_=window_->device();functions_=window_->vulkanInstance()->deviceFunctions(device_);VkDescriptorSetLayoutBinding binding{};binding.binding=0;binding.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_VERTEX_BIT;VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=1;layout.pBindings=&binding;if(functions_->vkCreateDescriptorSetLayout(device_,&layout,nullptr,&descriptorLayout_)!=VK_SUCCESS)return;
    VkPipelineLayoutCreateInfo pipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipeline.setLayoutCount=1;pipeline.pSetLayouts=&descriptorLayout_;if(functions_->vkCreatePipelineLayout(device_,&pipeline,nullptr,&pipelineLayout_)!=VK_SUCCESS)return;const int frames=window_->concurrentFrameCount();uniformBuffers_.resize(frames);descriptorSets_.resize(frames);VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,static_cast<uint32_t>(frames)};VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=frames;pool.poolSizeCount=1;pool.pPoolSizes=&poolSize;if(functions_->vkCreateDescriptorPool(device_,&pool,nullptr,&descriptorPool_)!=VK_SUCCESS)return;std::vector<VkDescriptorSetLayout> layouts(frames,descriptorLayout_);VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=descriptorPool_;allocate.descriptorSetCount=frames;allocate.pSetLayouts=layouts.data();if(functions_->vkAllocateDescriptorSets(device_,&allocate,descriptorSets_.data())!=VK_SUCCESS)return;
    for(int i=0;i<frames;++i){if(!createBuffer(sizeof(CloudCameraUniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,&uniformBuffers_[i]))return;VkDescriptorBufferInfo bufferInfo{uniformBuffers_[i].buffer,0,sizeof(CloudCameraUniform)};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=descriptorSets_[i];write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&bufferInfo;functions_->vkUpdateDescriptorSets(device_,1,&write,0,nullptr);}
    VkShaderModule pv=loadShader(QStringLiteral(":/point_cloud_shaders/cloud_points.vert.spv"));VkShaderModule pf=loadShader(QStringLiteral(":/point_cloud_shaders/cloud_points.frag.spv"));VkShaderModule lv=loadShader(QStringLiteral(":/point_cloud_shaders/cloud_lines.vert.spv"));VkShaderModule lf=loadShader(QStringLiteral(":/point_cloud_shaders/cloud_lines.frag.spv"));if(pv&&pf)pointPipeline_=createPipeline(pv,pf,true);if(lv&&lf)linePipeline_=createPipeline(lv,lf,false);for(VkShaderModule module:{pv,pf,lv,lf})if(module)functions_->vkDestroyShaderModule(device_,module,nullptr);initialized_=pointPipeline_&&linePipeline_;rebuildPointBuffer();rebuildLineBuffer();
}
void CloudVulkanRenderer::rebuildPointBuffer(){std::vector<PointCloudVertex> vertices;if(!window_->takeVertices(&vertices))return;pointCount_=static_cast<uint32_t>(std::min<size_t>(vertices.size(),std::numeric_limits<uint32_t>::max()));if(pointCount_==0)return;upload(&pointBuffer_,vertices.data(),vertices.size()*sizeof(PointCloudVertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);}
void CloudVulkanRenderer::rebuildLineBuffer(){const auto lines=window_->lineVertices();lineCount_=static_cast<uint32_t>(lines.size());if(lineCount_)upload(&lineBuffer_,lines.data(),lines.size()*sizeof(CloudLineVertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);}
void CloudVulkanRenderer::startNextFrame(){
    if(!initialized_){window_->frameReady();return;}rebuildPointBuffer();if(window_->takeLinesDirty())rebuildLineBuffer();const int frame=window_->currentFrame();CloudCameraUniform uniform;const QMatrix4x4 mvp=window_->viewProjection();std::memcpy(uniform.mvp,mvp.constData(),sizeof(uniform.mvp));if(const auto& doc=window_->document()){uniform.pixelScale=static_cast<float>(doc->settings.pixelScale);uniform.maximumHeight=static_cast<float>(doc->settings.maximumHeight);uniform.invertHeight=doc->settings.invertHeight?1.0F:0.0F;uniform.startColor[0]=doc->settings.startColor.redF();uniform.startColor[1]=doc->settings.startColor.greenF();uniform.startColor[2]=doc->settings.startColor.blueF();uniform.endColor[0]=doc->settings.endColor.redF();uniform.endColor[1]=doc->settings.endColor.greenF();uniform.endColor[2]=doc->settings.endColor.blueF();}const QVector3D origin=window_->origin();uniform.originX=origin.x();uniform.originY=origin.y();uniform.originZ=origin.z();uniform.viewportSize[0]=static_cast<float>(std::max(1,window_->swapChainImageSize().width()));uniform.viewportSize[1]=static_cast<float>(std::max(1,window_->swapChainImageSize().height()));upload(&uniformBuffers_[frame],&uniform,sizeof(uniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    VkClearValue clears[2]{};clears[0].color={{0.025F,0.035F,0.065F,1.0F}};clears[1].depthStencil={1.0F,0};VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=window_->defaultRenderPass();begin.framebuffer=window_->currentFramebuffer();begin.renderArea.extent={static_cast<uint32_t>(window_->swapChainImageSize().width()),static_cast<uint32_t>(window_->swapChainImageSize().height())};begin.clearValueCount=2;begin.pClearValues=clears;VkCommandBuffer command=window_->currentCommandBuffer();functions_->vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);VkViewport viewport{0,0,float(begin.renderArea.extent.width),float(begin.renderArea.extent.height),0,1};VkRect2D scissor{{0,0},begin.renderArea.extent};functions_->vkCmdSetViewport(command,0,1,&viewport);functions_->vkCmdSetScissor(command,0,1,&scissor);functions_->vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout_,0,1,&descriptorSets_[frame],0,nullptr);const VkDeviceSize offset=0;
    if(pointCount_&&pointBuffer_.buffer){functions_->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pointPipeline_);functions_->vkCmdBindVertexBuffers(command,0,1,&pointBuffer_.buffer,&offset);functions_->vkCmdDraw(command,6,pointCount_,0,0);}if(lineCount_&&lineBuffer_.buffer){functions_->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,linePipeline_);functions_->vkCmdBindVertexBuffers(command,0,1,&lineBuffer_.buffer,&offset);functions_->vkCmdDraw(command,lineCount_,1,0,0);}functions_->vkCmdEndRenderPass(command);window_->frameReady();
}
void CloudVulkanRenderer::releaseResources(){if(!functions_)return;for(auto& buffer:uniformBuffers_)destroyBuffer(&buffer);destroyBuffer(&pointBuffer_);destroyBuffer(&lineBuffer_);if(pointPipeline_)functions_->vkDestroyPipeline(device_,pointPipeline_,nullptr);if(linePipeline_)functions_->vkDestroyPipeline(device_,linePipeline_,nullptr);if(pipelineLayout_)functions_->vkDestroyPipelineLayout(device_,pipelineLayout_,nullptr);if(descriptorPool_)functions_->vkDestroyDescriptorPool(device_,descriptorPool_,nullptr);if(descriptorLayout_)functions_->vkDestroyDescriptorSetLayout(device_,descriptorLayout_,nullptr);initialized_=false;functions_=nullptr;device_=VK_NULL_HANDLE;}
} // namespace
#endif

class PointCloudWidget::Backend {
public:
    explicit Backend(PointCloudWidget* owner,QWidget* host):owner_(owner){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        instance_=std::make_unique<QVulkanInstance>();if(!instance_->create()){error_=QStringLiteral("Vulkan 初始化失败（错误码 %1）。").arg(instance_->errorCode());return;}window_=new CloudVulkanWindow;window_->setObjectName(QStringLiteral("pointCloudVulkanWindow"));window_->setVulkanInstance(instance_.get());if(window_->availablePhysicalDevices().isEmpty()){delete window_;window_=nullptr;error_=QStringLiteral("未找到可用于 PointCloud 的 Vulkan 图形设备。");return;}window_->viewChanged=[owner](float x,float y,float scale,float spanX,float spanY){owner->updateVisibleLod(x,y,scale,spanX,spanY);};container_=QWidget::createWindowContainer(window_,host);container_->setFocusPolicy(Qt::StrongFocus);
#else
        Q_UNUSED(host);error_=QStringLiteral("此构建未启用 Vulkan PointCloud。请安装 Vulkan SDK 和 Qt ShaderTools 后重新配置。");
#endif
    }
    ~Backend(){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        delete container_;container_=nullptr;window_=nullptr;
#endif
    }
    bool available()const{
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        return window_!=nullptr;
#else
        return false;
#endif
    }
    QWidget* widget()const{
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        return container_;
#else
        return nullptr;
#endif
    }
    QString error()const{return error_;}
    void setDocument(const PointCloudDocumentPtr& document){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->setDocument(document);
#else
        Q_UNUSED(document);
#endif
    }
    void setVertices(std::vector<PointCloudVertex> vertices){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->setVertices(std::move(vertices));
#else
        Q_UNUSED(vertices);
#endif
    }
    void fitAll(){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->fitAll();
#endif
    }
    void setGuides(bool visible){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        if(window_)window_->setGuidesVisible(visible);
#else
        Q_UNUSED(visible);
#endif
    }
    QImage grabFrame(){
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
        return window_?window_->grab():QImage();
#else
        return {};
#endif
    }
private:
    PointCloudWidget* owner_=nullptr;QString error_;
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
    std::unique_ptr<QVulkanInstance> instance_;CloudVulkanWindow* window_=nullptr;QWidget* container_=nullptr;
#endif
};

PointCloudWidget::PointCloudWidget(QWidget* parent):QWidget(parent){
    setObjectName(QStringLiteral("pointCloudWidget"));auto* root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(4);auto* tools=new QHBoxLayout;fitButton_=new QPushButton(QStringLiteral("视角复位 (Space)"));fitButton_->setObjectName(QStringLiteral("pointCloudFitAllButton"));guidesCheck_=new QCheckBox(QStringLiteral("坐标轴与网格"));guidesCheck_->setObjectName(QStringLiteral("pointCloudGuidesCheck"));guidesCheck_->setChecked(true);statusLabel_=new QLabel(QStringLiteral("未生成 PointCloud"));statusLabel_->setObjectName(QStringLiteral("pointCloudStatus"));tools->addWidget(fitButton_);tools->addWidget(guidesCheck_);tools->addStretch();tools->addWidget(statusLabel_);root->addLayout(tools);backend_=new Backend(this,this);if(backend_->available())root->addWidget(backend_->widget(),1);else{auto* error=new QLabel(backend_->error());error->setObjectName(QStringLiteral("pointCloudVulkanError"));error->setAlignment(Qt::AlignCenter);error->setWordWrap(true);error->setStyleSheet(QStringLiteral("QLabel { color: #ff8a80; background: #25282b; padding: 24px; }"));root->addWidget(error,1);}auto* resetShortcut=new QShortcut(QKeySequence(Qt::Key_Space),this);resetShortcut->setObjectName(QStringLiteral("pointCloudResetShortcut"));resetShortcut->setContext(Qt::WidgetWithChildrenShortcut);connect(resetShortcut,&QShortcut::activated,this,&PointCloudWidget::fitAll);connect(fitButton_,&QPushButton::clicked,this,&PointCloudWidget::fitAll);connect(guidesCheck_,&QCheckBox::toggled,this,[this](bool visible){backend_->setGuides(visible);});fitButton_->setEnabled(backend_->available());guidesCheck_->setEnabled(backend_->available());
}
PointCloudWidget::~PointCloudWidget(){if(requestToken_)requestToken_->cancel();delete backend_;}
bool PointCloudWidget::isVulkanAvailable()const noexcept{return backend_&&backend_->available();}
void PointCloudWidget::clear(){setDocument({});}
void PointCloudWidget::setDocument(const PointCloudDocumentPtr& document){if(requestToken_)requestToken_->cancel();requestToken_=std::make_shared<CancellationToken>();document_=document;tileCache_.clear();pendingTiles_.clear();visibleLevel_=-1;visibleTileRange_={};visiblePixelRect_={};backend_->setDocument(document_);if(!document_){backend_->setVertices({});statusLabel_->setText(QStringLiteral("未生成 PointCloud"));return;}rebuildVisiblePoints();statusLabel_->setText(QStringLiteral("%1 × %2 | 初始 LOD %3").arg(document_->source->metadata.width).arg(document_->source->metadata.height).arg(document_->rootLevel));}
void PointCloudWidget::fitAll(){if(backend_)backend_->fitAll();}
QImage PointCloudWidget::grabFrame(){return backend_?backend_->grabFrame():QImage();}
QString PointCloudWidget::tileKey(int level,int tileX,int tileY)const{return QStringLiteral("%1:%2:%3:%4").arg(document_?document_->generation:0).arg(level).arg(tileX).arg(tileY);}

void PointCloudWidget::updateVisibleLod(float centerX,float centerY,float worldPerPixel,float spanX,float spanY){
    if(!document_||!document_->source||document_->settings.pixelScale<=0.0)return;const double pixelScale=document_->settings.pixelScale;const double desiredStep=std::max(1.0,double(worldPerPixel)*2.0/pixelScale);int level=static_cast<int>(std::floor(std::log2(desiredStep)));level=std::clamp(level,0,document_->rootLevel);const double originX=std::max<qint64>(0,document_->source->metadata.width-1)*pixelScale*0.5;const double originY=std::max<qint64>(0,document_->source->metadata.height-1)*pixelScale*0.5;const double pixelCenterX=(centerX+originX)/pixelScale;const double pixelCenterY=(centerY+originY)/pixelScale;const double halfWidth=std::max(256.0,double(spanX)/pixelScale*0.75);const double halfHeight=std::max(256.0,double(spanY)/pixelScale*0.75);QRectF pixelRect(pixelCenterX-halfWidth,pixelCenterY-halfHeight,halfWidth*2.0,halfHeight*2.0);pixelRect=pixelRect.intersected(QRectF(0,0,document_->source->metadata.width,document_->source->metadata.height));
    QRect tileRange;while(true){const qint64 tileSpan=qint64(kPointCloudTileSamples)*(qint64(1)<<level);const int left=std::max(0,static_cast<int>(std::floor(pixelRect.left()/tileSpan)));const int top=std::max(0,static_cast<int>(std::floor(pixelRect.top()/tileSpan)));const int right=std::max(left,static_cast<int>(std::floor(std::max(0.0,pixelRect.right()-1.0)/tileSpan)));const int bottom=std::max(top,static_cast<int>(std::floor(std::max(0.0,pixelRect.bottom()-1.0)/tileSpan)));tileRange=QRect(QPoint(left,top),QPoint(right,bottom));const qint64 points=qint64(tileRange.width())*tileRange.height()*kPointCloudTileSamples*kPointCloudTileSamples;if(points<=kPointCloudRootPointBudget||level>=document_->rootLevel)break;++level;}
    if(level==visibleLevel_&&tileRange==visibleTileRange_)return;visibleLevel_=level;visibleTileRange_=tileRange;visiblePixelRect_=pixelRect;requestVisibleTiles();rebuildVisiblePoints();
}
void PointCloudWidget::requestVisibleTiles(){
    if(!document_||visibleLevel_<0||visibleLevel_>=document_->rootLevel)return;const auto doc=document_;const auto token=requestToken_;const quint64 generation=doc->generation;for(int y=visibleTileRange_.top();y<=visibleTileRange_.bottom()&&pendingTiles_.size()<8;++y)for(int x=visibleTileRange_.left();x<=visibleTileRange_.right()&&pendingTiles_.size()<8;++x){const QString key=tileKey(visibleLevel_,x,y);if(tileCache_.contains(key)||pendingTiles_.contains(key))continue;pendingTiles_.insert(key);auto* watcher=new QFutureWatcher<QPair<PointCloudTilePtr,QString>>(this);connect(watcher,&QFutureWatcher<QPair<PointCloudTilePtr,QString>>::finished,this,[this,watcher,key,generation]{const auto result=watcher->result();watcher->deleteLater();pendingTiles_.remove(key);if(!document_||document_->generation!=generation||!result.first)return;const int cost=std::max(1,static_cast<int>(result.first->vertices.size()*sizeof(PointCloudVertex)/1024));tileCache_.insert(key,new PointCloudTile(*result.first),cost);rebuildVisiblePoints();requestVisibleTiles();});watcher->setFuture(QtConcurrent::run([doc,token,level=visibleLevel_,x,y]{QString error;auto tile=PointCloudBuilder::buildTile(doc,level,x,y,token,&error);return qMakePair(tile,error);}));}
}
void PointCloudWidget::rebuildVisiblePoints(){
    if(!document_){backend_->setVertices({});return;}std::vector<PointCloudVertex> points;size_t rootCount=0;for(const auto& tile:document_->rootTiles)if(tile)rootCount+=tile->vertices.size();points.reserve(static_cast<size_t>(std::min<qint64>(kPointCloudFramePointBudget,rootCount+kPointCloudRootPointBudget)));for(const auto& tile:document_->rootTiles)if(tile)points.insert(points.end(),tile->vertices.begin(),tile->vertices.end());if(visibleLevel_>=0&&visibleLevel_<document_->rootLevel){for(int y=visibleTileRange_.top();y<=visibleTileRange_.bottom();++y)for(int x=visibleTileRange_.left();x<=visibleTileRange_.right();++x){const auto* tile=tileCache_.object(tileKey(visibleLevel_,x,y));if(!tile)continue;const size_t remaining=static_cast<size_t>(kPointCloudFramePointBudget)-std::min<size_t>(points.size(),kPointCloudFramePointBudget);const size_t count=std::min(remaining,tile->vertices.size());points.insert(points.end(),tile->vertices.begin(),tile->vertices.begin()+static_cast<std::ptrdiff_t>(count));if(points.size()>=static_cast<size_t>(kPointCloudFramePointBudget))break;}}backend_->setVertices(std::move(points));statusLabel_->setText(QStringLiteral("LOD %1 | 缓存 %2 MB | 正在加载 %3").arg(visibleLevel_<0?document_->rootLevel:visibleLevel_).arg(tileCache_.totalCost()/1024.0,0,'f',1).arg(pendingTiles_.size()));
}

#include "vulkanwindow.h"

#if defined(GL3D_HAS_VULKAN)

#include "renderframe.h"
#include "meshloader.h"
#include <QVulkanDeviceFunctions>
#include <QVulkanInstance>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QVector>
#include <QVector4D>
#include <QScreen>
#include <QDebug>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <cstring>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace {

struct TextureResource {
    QImage source;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDeviceSize allocationSize = 0;
};

struct DrawRange {
    uint32_t firstIndex = 0, indexCount = 0;
    int32_t vertexOffset = 0;
    QVector3D color{0.7f, 0.7f, 0.7f};
    float metallic = 0.0f, roughness = 1.0f;
    TextureResource base, metalRough, normal;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
};

struct alignas(16) DrawUniform {
    float mvp[16];
    float modelView[16];
    float baseMetal[4];
    float materialFlags[4];
    float options[4];
};
static_assert(sizeof(DrawUniform) == 176, "Vulkan UBO must match std140 layout");
struct PushConstants { int mode = 0; float outline = 0.0f; };
struct OverlayVertex { float position[3]; float color[3]; };
struct SingleSampleTarget {
    VkImage depth = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocationSize = 0;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

struct PickTarget {
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
    VkDeviceMemory colorMemory = VK_NULL_HANDLE, depthMemory = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    VkDeviceSize colorAllocation = 0, depthAllocation = 0, readbackAllocation = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

static QByteArray readBinary(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static QImage loadTextureImage(const QByteArray& bytes, const QString& path, bool flipVertically)
{
    QImage image = !bytes.isEmpty() ? QImage::fromData(bytes) : QImage(path);
    if (image.isNull()) return {};
    image = image.convertToFormat(QImage::Format_RGBA8888);
    return flipVertically ? image.mirrored() : image;
}

class VulkanWindowRenderer final : public QVulkanWindowRenderer {
public:
    explicit VulkanWindowRenderer(VulkanWindow* window) : m_window(window) {}

    void initResources() override
    {
        m_device = m_window->device();
        m_df = m_window->vulkanInstance()->deviceFunctions(m_device);
        const QString dir = QCoreApplication::applicationDirPath() + "/shaders/";
        const QByteArray vertexBytes = readBinary(dir + "vulkan_model.vert.spv");
        const QByteArray fragmentBytes = readBinary(dir + "vulkan_model.frag.spv");
        m_vertexShader = createShaderModule(vertexBytes);
        m_fragmentShader = createShaderModule(fragmentBytes);
        m_pickFragmentShader = createShaderModule(readBinary(dir + "vulkan_pick.frag.spv"));
        m_overlayVertexShader = createShaderModule(readBinary(dir + "vulkan_overlay.vert.spv"));
        m_overlayFragmentShader = createShaderModule(readBinary(dir + "vulkan_overlay.frag.spv"));
        if (!m_vertexShader || !m_fragmentShader)
            qWarning().noquote() << "Vulkan shaders unavailable in" << dir
                                 << "vertex bytes:" << vertexBytes.size()
                                 << "fragment bytes:" << fragmentBytes.size();
        VkDescriptorSetLayoutBinding bindings[4]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        for (uint32_t binding = 1; binding < 4; ++binding)
            bindings[binding] = {binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 4;
        layout.pBindings = bindings;
        if (m_df->vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_descriptorLayout) != VK_SUCCESS)
            qWarning() << "Vulkan descriptor layout creation failed";
        const auto* properties = m_window->physicalDeviceProperties();
        const VkDeviceSize alignment = properties ? properties->limits.minUniformBufferOffsetAlignment : 256;
        m_uniformStride = (sizeof(DrawUniform) + alignment - 1) / alignment * alignment;
        if (properties)
            m_window->setDeviceDescription(QString::fromLatin1(properties->deviceName),
                QString("Vulkan %1.%2.%3").arg(VK_VERSION_MAJOR(properties->apiVersion))
                    .arg(VK_VERSION_MINOR(properties->apiVersion)).arg(VK_VERSION_PATCH(properties->apiVersion)));
        VkPhysicalDeviceFeatures features{};
        m_window->vulkanInstance()->functions()->vkGetPhysicalDeviceFeatures(m_window->physicalDevice(), &features);
        m_wireframeSupported = features.fillModeNonSolid == VK_TRUE;
        std::vector<OverlayVertex> lines;
        // 与 OpenGL 的坐标轴 HUD 保持相同：轴杆 + 8 段线框锥体箭头。
        const QVector3D axes[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const QVector3D colors[3]{{0.95f, 0.25f, 0.25f}, {0.25f, 0.95f, 0.25f}, {0.3f, 0.5f, 1.0f}};
        auto line = [&lines](const QVector3D& a, const QVector3D& b, const QVector3D& color) {
            lines.push_back({{a.x(), a.y(), a.z()}, {color.x(), color.y(), color.z()}});
            lines.push_back({{b.x(), b.y(), b.z()}, {color.x(), color.y(), color.z()}});
        };
        auto cone = [&line](const QVector3D& base, const QVector3D& axis,
                            float length, float radius, const QVector3D& color) {
            const QVector3D d = axis.normalized();
            const QVector3D tip = base + d * length;
            const QVector3D up = std::fabs(d.y()) > 0.99f
                ? QVector3D(1, 0, 0) : QVector3D(0, 1, 0);
            const QVector3D right = QVector3D::crossProduct(d, up).normalized();
            const QVector3D bitangent = QVector3D::crossProduct(d, right);
            constexpr int segments = 8;
            constexpr float pi = 3.14159265358979323846f;
            for (int i = 0; i < segments; ++i) {
                const float a0 = float(i) * 2.0f * pi / float(segments);
                const float a1 = float(i + 1) * 2.0f * pi / float(segments);
                const QVector3D p0 = base + right * (std::cos(a0) * radius)
                                           + bitangent * (std::sin(a0) * radius);
                const QVector3D p1 = base + right * (std::cos(a1) * radius)
                                           + bitangent * (std::sin(a1) * radius);
                line(p0, tip, color);
                line(p0, p1, color);
            }
        };
        for (int i = 0; i < 3; ++i) {
            const QVector3D base = axes[i] * 0.72f;
            line(QVector3D(), base, colors[i]);
            cone(base, axes[i], 0.28f, 0.055f, colors[i]);
        }
        m_arrowVertexCount = uint32_t(lines.size());
        for (int i = 0; i < 3; ++i) {
            const QVector3D seed = i == 1 ? axes[0] : axes[1];
            const QVector3D tangent = QVector3D::crossProduct(axes[i], seed).normalized();
            const QVector3D bitangent = QVector3D::crossProduct(axes[i], tangent).normalized();
            for (int segment = 0; segment < 64; ++segment) {
                const float a = float(segment) * 6.2831853f / 64.0f;
                const float b = float(segment + 1) * 6.2831853f / 64.0f;
                line(tangent * std::cos(a) + bitangent * std::sin(a),
                     tangent * std::cos(b) + bitangent * std::sin(b), colors[i]);
            }
        }
        m_ringVertexCount = uint32_t(lines.size()) - m_arrowVertexCount;
        for (int i = 0; i < 3; ++i) {
            line(QVector3D(), axes[i], colors[i]);
            const QVector3D sideA = axes[(i + 1) % 3] * 0.07f;
            const QVector3D sideB = axes[(i + 2) % 3] * 0.07f;
            const QVector3D tip = axes[i];
            line(tip - sideA - sideB, tip + sideA - sideB, colors[i]);
            line(tip + sideA - sideB, tip + sideA + sideB, colors[i]);
            line(tip + sideA + sideB, tip - sideA + sideB, colors[i]);
            line(tip - sideA + sideB, tip - sideA - sideB, colors[i]);
        }
        m_scaleVertexCount = uint32_t(lines.size()) - m_arrowVertexCount - m_ringVertexCount;
        createBuffer(lines.size() * sizeof(OverlayVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     lines.data(), m_overlayBuffer, m_overlayMemory, &m_overlayAllocation);
    }

    void initSwapChainResources() override {
        createSingleSampleTargets();
        createPickTarget();
        createPipeline();
        if (m_singleSamplePass) createPipeline(true);
        if (m_pick.pass) createPipeline(false, true);
        createOverlayPipeline();
        if (m_singleSamplePass) createOverlayPipeline(true);
        createOverlayPipeline(false, true);
        if (m_singleSamplePass) createOverlayPipeline(true, true);
        m_pipelineRevision = m_window->pipelineRevision();
    }

    void releaseSwapChainResources() override {
        destroyPipeline();
        destroyPickTarget();
        destroySingleSampleTargets();
    }

    void releaseResources() override
    {
        if (!m_df) return;
        m_df->vkDeviceWaitIdle(m_device);
        destroyPipeline();
        destroyPickTarget();
        destroySingleSampleTargets();
        if (m_vertexShader) m_df->vkDestroyShaderModule(m_device, m_vertexShader, nullptr);
        if (m_fragmentShader) m_df->vkDestroyShaderModule(m_device, m_fragmentShader, nullptr);
        if (m_pickFragmentShader) m_df->vkDestroyShaderModule(m_device, m_pickFragmentShader, nullptr);
        if (m_overlayVertexShader) m_df->vkDestroyShaderModule(m_device, m_overlayVertexShader, nullptr);
        if (m_overlayFragmentShader) m_df->vkDestroyShaderModule(m_device, m_overlayFragmentShader, nullptr);
        destroyBuffer(m_vertexBuffer, m_vertexMemory);
        destroyBuffer(m_indexBuffer, m_indexMemory);
        destroyBuffer(m_overlayBuffer, m_overlayMemory);
        destroyBuffer(m_gridBuffer, m_gridMemory);
        destroyModelDescriptors();
        destroyTextures();
        destroyTexture(m_whiteTexture);
        if (m_descriptorLayout) m_df->vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
        m_vertexShader = VK_NULL_HANDLE;
        m_fragmentShader = VK_NULL_HANDLE;
        m_pickFragmentShader = VK_NULL_HANDLE;
        m_overlayVertexShader = m_overlayFragmentShader = VK_NULL_HANDLE;
        m_descriptorLayout = VK_NULL_HANDLE;
        m_df = nullptr;
    }

    void startNextFrame() override
    {
        createModelBuffers();
        if (m_uploadedGridRevision != m_window->gridRevision()) rebuildGrid();
        if (m_pipelineRevision != m_window->pipelineRevision()) {
            m_df->vkDeviceWaitIdle(m_device);
            destroyPipeline();
            createPipeline();
            if (m_singleSamplePass) createPipeline(true);
            if (m_pick.pass) createPipeline(false, true);
            createOverlayPipeline();
            if (m_singleSamplePass) createOverlayPipeline(true);
            createOverlayPipeline(false, true);
            if (m_singleSamplePass) createOverlayPipeline(true, true);
            m_pipelineRevision = m_window->pipelineRevision();
        }
        if (m_window->scene().hasModel() && m_uploadedTextureRevision != m_window->textureRevision())
            rebuildTextures();
        QPoint pickPosition;
        if (m_window->takePendingPick(pickPosition)) {
            const int index = pickAt(pickPosition);
            m_window->selectSubMesh(index == -2 ? m_window->pickSubMesh(pickPosition) : index);
        }
        const QSize size = m_window->swapChainImageSize();
        VkCommandBuffer cmd = m_window->currentCommandBuffer();
        const int imageIndex = m_window->currentSwapChainImageIndex();
        const bool singleSample = !m_window->scene().options().antialiasing &&
            m_singleSamplePass && m_singleSamplePipeline &&
            imageIndex >= 0 && imageIndex < int(m_singleSampleTargets.size());
        VkPipeline modelPipeline = singleSample ? m_singleSamplePipeline : m_pipeline;
        VkPipeline outlinePipeline = singleSample ? m_singleSampleOutlinePipeline : m_outlinePipeline;
        VkPipeline selectionPipeline = singleSample ? m_singleSampleSelectionPipeline : m_selectionPipeline;
        VkPipeline overlayPipeline = singleSample ? m_singleSampleOverlayPipeline : m_overlayPipeline;
        VkPipeline gridPipeline = singleSample ? m_singleSampleGridPipeline : m_gridPipeline;
        VkClearValue clearValues[3]{};
        const float* background = m_window->clearColor();
        clearValues[0].color = {{background[0], background[1], background[2], 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0};
        clearValues[2] = clearValues[0]; // MSAA 时第三个附件是多重采样颜色。
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = singleSample ? m_singleSamplePass : m_window->defaultRenderPass();
        pass.framebuffer = singleSample ? m_singleSampleTargets[size_t(imageIndex)].framebuffer
                                        : m_window->currentFramebuffer();
        pass.renderArea.extent = {uint32_t(size.width()), uint32_t(size.height())};
        pass.clearValueCount = singleSample || m_window->sampleCountFlagBits() == VK_SAMPLE_COUNT_1_BIT ? 2 : 3;
        pass.pClearValues = clearValues;
        m_df->vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        int drawCalls = 0;
        const RenderFrame frame = makeRenderFrame(m_window->scene(), size);
        VkViewport viewport{0.0f, 0.0f, float(size.width()), float(size.height()), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {uint32_t(size.width()), uint32_t(size.height())}};
        const VkDeviceSize offset = 0;
        m_df->vkCmdSetViewport(cmd, 0, 1, &viewport);
        m_df->vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (m_window->scene().options().showGrid && gridPipeline && m_gridBuffer && m_gridVertexCount) {
            const QMatrix4x4 mvp = m_window->clipCorrectionMatrix() * frame.projection * frame.view;
            m_df->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline);
            m_df->vkCmdBindVertexBuffers(cmd, 0, 1, &m_gridBuffer, &offset);
            m_df->vkCmdPushConstants(cmd, m_overlayPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                     0, 64, mvp.constData());
            m_df->vkCmdDraw(cmd, m_gridVertexCount, 1, 0, 0);
            ++drawCalls;
        }
        if (modelPipeline && m_indexBuffer && m_uniformBuffer && m_window->scene().hasModel()) {
            const RenderOptions& options = m_window->scene().options();
            const VkDeviceSize frameOffset = m_uniformStride * m_ranges.size() * m_window->currentFrame();
            void* mapped = nullptr;
            if (m_df->vkMapMemory(m_device, m_uniformMemory, 0, m_uniformAllocation, 0, &mapped) == VK_SUCCESS) {
                for (int index = 0; index < m_ranges.size(); ++index) {
                    const DrawRange& range = m_ranges[index];
                    const QMatrix4x4 modelView = frame.view * m_window->subMeshTransform(index);
                    DrawUniform uniform{};
                    const QMatrix4x4 mvp = m_window->clipCorrectionMatrix() * frame.projection * modelView;
                    std::memcpy(uniform.mvp, mvp.constData(), sizeof(uniform.mvp));
                    std::memcpy(uniform.modelView, modelView.constData(), sizeof(uniform.modelView));
                    uniform.baseMetal[0] = range.color.x(); uniform.baseMetal[1] = range.color.y();
                    uniform.baseMetal[2] = range.color.z(); uniform.baseMetal[3] = range.metallic;
                    uniform.materialFlags[0] = range.roughness;
                    uniform.materialFlags[1] = range.base.source.isNull() ? 0.0f : 1.0f;
                    uniform.materialFlags[2] = range.metalRough.source.isNull() ? 0.0f : 1.0f;
                    uniform.materialFlags[3] = range.normal.source.isNull() ? 0.0f : 1.0f;
                    uniform.options[0] = options.pbr ? 1.0f : 0.0f;
                    uniform.options[1] = options.normalMap ? 1.0f : 0.0f;
                    uniform.options[2] = float(options.debugView);
                    std::memcpy(static_cast<char*>(mapped) + frameOffset + m_uniformStride * index,
                                &uniform, sizeof(uniform));
                }
                m_df->vkUnmapMemory(m_device, m_uniformMemory);
            } else {
                qWarning() << "Vulkan uniform buffer mapping failed";
            }
            m_df->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, modelPipeline);
            m_df->vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer, &offset);
            m_df->vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            PushConstants constants{};
            m_df->vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                     0, sizeof(constants), &constants);
            for (int index = 0; index < m_ranges.size(); ++index) {
                const DrawRange& range = m_ranges[index];
                if (!m_window->subMeshVisible(index) || !range.descriptor) continue;
                const uint32_t dynamicOffset = uint32_t(frameOffset + m_uniformStride * index);
                m_df->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0,
                                              1, &range.descriptor, 1, &dynamicOffset);
                m_df->vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, range.vertexOffset, 0);
                ++drawCalls;
            }
            const int selected = m_window->selectedSubMesh();
            if (selected >= 0 && selected < m_ranges.size() && m_window->subMeshVisible(selected) &&
                options.showSelection && m_ranges[selected].descriptor) {
                const DrawRange& range = m_ranges[selected];
                const uint32_t dynamicOffset = uint32_t(frameOffset + m_uniformStride * selected);
                m_df->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0,
                                              1, &range.descriptor, 1, &dynamicOffset);
                if (outlinePipeline && options.outlineWidth > 0.0f) {
                    constants.mode = 1;
                    constants.outline = options.outlineWidth;
                    m_df->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, outlinePipeline);
                    m_df->vkCmdPushConstants(cmd, m_pipelineLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
                    m_df->vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, range.vertexOffset, 0);
                    ++drawCalls;
                }
                constants.mode = 2;
                constants.outline = 0.0f;
                m_df->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        selectionPipeline ? selectionPipeline : modelPipeline);
                m_df->vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                         0, sizeof(constants), &constants);
                m_df->vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, range.vertexOffset, 0);
                ++drawCalls;
            }
            if (selected >= 0 && m_window->transformMode() && overlayPipeline && m_overlayBuffer) {
                const auto& centers = m_window->scene().selection().centers();
                const QVector3D position = m_window->subMeshTransform(selected).map(centers[size_t(selected)]);
                QMatrix4x4 model;
                model.translate(position);
                if (m_window->transformMode() == 3) model.rotate(m_window->subMeshRotation(selected));
                model.scale(m_window->gizmoRadius());
                const QMatrix4x4 mvp = m_window->clipCorrectionMatrix() * frame.projection * frame.view * model;
                float pushMatrix[16];
                std::memcpy(pushMatrix, mvp.constData(), sizeof(pushMatrix));
                m_df->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline);
                m_df->vkCmdBindVertexBuffers(cmd, 0, 1, &m_overlayBuffer, &offset);
                m_df->vkCmdPushConstants(cmd, m_overlayPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                         0, sizeof(pushMatrix), pushMatrix);
                const uint32_t first = m_window->transformMode() == 2 ? m_arrowVertexCount
                    : m_window->transformMode() == 3 ? m_arrowVertexCount + m_ringVertexCount : 0;
                const uint32_t count = m_window->transformMode() == 2 ? m_ringVertexCount
                    : m_window->transformMode() == 3 ? m_scaleVertexCount : m_arrowVertexCount;
                m_df->vkCmdDraw(cmd, count, 1, first, 0);
                ++drawCalls;
            }
        } else if (m_window->scene().hasModel() && !m_reportedDrawFailure) {
            qWarning() << "Vulkan model draw skipped: pipeline=" << bool(m_pipeline)
                       << "indexBuffer=" << bool(m_indexBuffer)
                       << "ranges=" << m_ranges.size()
                       << "vertices=" << m_uploadedVertexCount
                       << "indices=" << m_uploadedIndexCount;
            m_reportedDrawFailure = true;
        }
        if (m_window->scene().hasModel() && m_window->scene().options().showAxes &&
            overlayPipeline && m_overlayBuffer) {
            QMatrix4x4 viewRotation = frame.view;
            viewRotation.setColumn(3, QVector4D(0, 0, 0, 1));
            QMatrix4x4 orthographic;
            orthographic.ortho(-1, 1, -1, 1, -100, 100);
            QMatrix4x4 corner;
            corner.translate(-0.8f, -0.8f);
            corner.scale(0.2f);
            const QMatrix4x4 mvp = m_window->clipCorrectionMatrix() * corner *
                                    orthographic * viewRotation;
            m_df->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline);
            m_df->vkCmdBindVertexBuffers(cmd, 0, 1, &m_overlayBuffer, &offset);
            m_df->vkCmdPushConstants(cmd, m_overlayPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                     0, 64, mvp.constData());
            m_df->vkCmdDraw(cmd, m_arrowVertexCount, 1, 0, 0);
            ++drawCalls;
        }
        m_df->vkCmdEndRenderPass(cmd);
        m_window->recordFrame(drawCalls);
        m_window->frameReady();
    }

private:
    uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
    {
        VkPhysicalDeviceMemoryProperties properties{};
        m_window->vulkanInstance()->functions()->vkGetPhysicalDeviceMemoryProperties(
            m_window->physicalDevice(), &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        return UINT32_MAX;
    }

    void destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory)
    {
        if (buffer) m_df->vkDestroyBuffer(m_device, buffer, nullptr);
        if (memory) m_df->vkFreeMemory(m_device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data,
                      VkBuffer& buffer, VkDeviceMemory& memory, VkDeviceSize* allocated = nullptr)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const VkResult createResult = m_df->vkCreateBuffer(m_device, &info, nullptr, &buffer);
        if (createResult != VK_SUCCESS) {
            qWarning() << "vkCreateBuffer failed:" << createResult << "size=" << size;
            return false;
        }
        VkMemoryRequirements requirements{};
        m_df->vkGetBufferMemoryRequirements(m_device, buffer, &requirements);
        if (allocated) *allocated = requirements.size;
        const uint32_t type = findMemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) {
            qWarning() << "No suitable Vulkan memory type for buffer size" << size;
            destroyBuffer(buffer, memory); return false;
        }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
        const VkResult allocateResult = m_df->vkAllocateMemory(m_device, &allocate, nullptr, &memory);
        const VkResult bindResult = allocateResult == VK_SUCCESS
            ? m_df->vkBindBufferMemory(m_device, buffer, memory, 0) : allocateResult;
        if (allocateResult != VK_SUCCESS || bindResult != VK_SUCCESS) {
            qWarning() << "Vulkan buffer memory failed: allocate=" << allocateResult
                       << "bind=" << bindResult << "size=" << size;
            destroyBuffer(buffer, memory); return false;
        }
        if (data) {
            void* mapped = nullptr;
            const VkResult mapResult = m_df->vkMapMemory(m_device, memory, 0, size, 0, &mapped);
            if (mapResult != VK_SUCCESS) {
                qWarning() << "vkMapMemory failed:" << mapResult << "size=" << size;
                destroyBuffer(buffer, memory); return false;
            }
            std::memcpy(mapped, data, size_t(size));
            m_df->vkUnmapMemory(m_device, memory);
        }
        return true;
    }

    void rebuildGrid()
    {
        m_df->vkDeviceWaitIdle(m_device);
        destroyBuffer(m_gridBuffer, m_gridMemory);
        m_gridVertexCount = 0;
        m_gridAllocation = 0;
        const Mesh& mesh = m_window->scene().mesh();
        const float radius = mesh.radius();
        if (m_window->scene().hasModel() && radius > 0.0f) {
            std::vector<OverlayVertex> vertices;
            const float* clear = m_window->clearColor();
            const float luminance = (clear[0] + clear[1] + clear[2]) / 3.0f;
            const QVector3D base = luminance > 0.5f
                ? QVector3D(0.45f, 0.45f, 0.5f) : QVector3D(0.32f, 0.32f, 0.38f);
            auto line = [&vertices](const QVector3D& a, const QVector3D& b, const QVector3D& color) {
                vertices.push_back({{a.x(), a.y(), a.z()}, {color.x(), color.y(), color.z()}});
                vertices.push_back({{b.x(), b.y(), b.z()}, {color.x(), color.y(), color.z()}});
            };
            const float extent = radius * 1.5f;
            const float z = -extent * 0.001f;
            for (int i = 0; i <= 8; ++i) {
                const float p = -extent + i * extent * 0.25f;
                line({-extent, p, z}, {extent, p, z}, base);
                line({p, -extent, z}, {p, extent, z}, base);
            }
            line({-extent, 0, z}, {extent, 0, z}, base * 1.7f);
            line({0, -extent, z}, {0, extent, z}, base * 1.7f);
            if (createBuffer(vertices.size() * sizeof(OverlayVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             vertices.data(), m_gridBuffer, m_gridMemory, &m_gridAllocation))
                m_gridVertexCount = uint32_t(vertices.size());
        }
        m_uploadedGridRevision = m_window->gridRevision();
    }

    void destroyTexture(TextureResource& texture)
    {
        if (texture.sampler) m_df->vkDestroySampler(m_device, texture.sampler, nullptr);
        if (texture.view) m_df->vkDestroyImageView(m_device, texture.view, nullptr);
        if (texture.image) m_df->vkDestroyImage(m_device, texture.image, nullptr);
        if (texture.memory) m_df->vkFreeMemory(m_device, texture.memory, nullptr);
        texture.sampler = VK_NULL_HANDLE;
        texture.view = VK_NULL_HANDLE;
        texture.image = VK_NULL_HANDLE;
        texture.memory = VK_NULL_HANDLE;
        texture.allocationSize = 0;
    }

    void destroyTextures()
    {
        for (DrawRange& range : m_ranges) {
            destroyTexture(range.base);
            destroyTexture(range.metalRough);
            destroyTexture(range.normal);
        }
    }

    void destroyModelDescriptors()
    {
        if (m_descriptorPool) m_df->vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        destroyBuffer(m_uniformBuffer, m_uniformMemory);
        m_descriptorPool = VK_NULL_HANDLE;
        m_uniformAllocation = 0;
        for (DrawRange& range : m_ranges) range.descriptor = VK_NULL_HANDLE;
    }

    bool createTexture(TextureResource& texture)
    {
        if (texture.source.isNull()) return true;
        QImage image = texture.source.convertToFormat(QImage::Format_RGBA8888);
        std::vector<QImage> levels;
        levels.push_back(image);
        if (m_window->mipmapsEnabled()) {
            while (image.width() > 1 || image.height() > 1) {
                image = image.scaled(std::max(1, image.width() / 2), std::max(1, image.height() / 2),
                                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                levels.push_back(image);
            }
        }
        VkDeviceSize byteCount = 0;
        std::vector<VkBufferImageCopy> copies;
        copies.reserve(levels.size());
        for (uint32_t level = 0; level < levels.size(); ++level) {
            const QImage& part = levels[level];
            VkBufferImageCopy copy{};
            copy.bufferOffset = byteCount;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            copy.imageExtent = {uint32_t(part.width()), uint32_t(part.height()), 1};
            copies.push_back(copy);
            byteCount += VkDeviceSize(part.bytesPerLine()) * part.height();
        }
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        if (!createBuffer(byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr, staging, stagingMemory))
            return false;
        void* mapped = nullptr;
        if (m_df->vkMapMemory(m_device, stagingMemory, 0, byteCount, 0, &mapped) != VK_SUCCESS) {
            destroyBuffer(staging, stagingMemory);
            return false;
        }
        for (size_t level = 0; level < levels.size(); ++level)
            std::memcpy(static_cast<char*>(mapped) + copies[level].bufferOffset,
                        levels[level].constBits(), size_t(levels[level].bytesPerLine()) * levels[level].height());
        m_df->vkUnmapMemory(m_device, stagingMemory);

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {uint32_t(levels[0].width()), uint32_t(levels[0].height()), 1};
        imageInfo.mipLevels = uint32_t(levels.size());
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (m_df->vkCreateImage(m_device, &imageInfo, nullptr, &texture.image) != VK_SUCCESS) {
            destroyBuffer(staging, stagingMemory); return false;
        }
        VkMemoryRequirements requirements{};
        m_df->vkGetImageMemoryRequirements(m_device, texture.image, &requirements);
        const uint32_t memoryType = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        if (memoryType == UINT32_MAX ||
            m_df->vkAllocateMemory(m_device, &allocation, nullptr, &texture.memory) != VK_SUCCESS ||
            m_df->vkBindImageMemory(m_device, texture.image, texture.memory, 0) != VK_SUCCESS) {
            destroyBuffer(staging, stagingMemory);
            destroyTexture(texture);
            return false;
        }
        texture.allocationSize = requirements.size;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = texture.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = imageInfo.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, imageInfo.mipLevels, 0, 1};
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.maxLod = float(levels.size());
        sampler.maxAnisotropy = 1.0f;
        if (m_df->vkCreateImageView(m_device, &view, nullptr, &texture.view) != VK_SUCCESS ||
            m_df->vkCreateSampler(m_device, &sampler, nullptr, &texture.sampler) != VK_SUCCESS) {
            destroyBuffer(staging, stagingMemory);
            destroyTexture(texture);
            return false;
        }

        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = m_window->graphicsCommandPool();
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        if (m_df->vkAllocateCommandBuffers(m_device, &commandInfo, &command) != VK_SUCCESS) {
            destroyBuffer(staging, stagingMemory); destroyTexture(texture); return false;
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_df->vkBeginCommandBuffer(command, &begin);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image = texture.image;
        barrier.subresourceRange = view.subresourceRange;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        m_df->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 0, nullptr, 0, nullptr, 1, &barrier);
        m_df->vkCmdCopyBufferToImage(command, staging, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     uint32_t(copies.size()), copies.data());
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        m_df->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                   0, 0, nullptr, 0, nullptr, 1, &barrier);
        const VkResult endResult = m_df->vkEndCommandBuffer(command);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        const VkResult submitResult = endResult == VK_SUCCESS
            ? m_df->vkQueueSubmit(m_window->graphicsQueue(), 1, &submit, VK_NULL_HANDLE) : endResult;
        if (submitResult == VK_SUCCESS) m_df->vkQueueWaitIdle(m_window->graphicsQueue());
        m_df->vkFreeCommandBuffers(m_device, commandInfo.commandPool, 1, &command);
        destroyBuffer(staging, stagingMemory);
        if (submitResult != VK_SUCCESS) { destroyTexture(texture); return false; }
        return true;
    }

    bool createModelDescriptors()
    {
        if (m_ranges.isEmpty() || !m_descriptorLayout) return false;
        const uint32_t count = uint32_t(m_ranges.size());
        const VkDeviceSize bytes = m_uniformStride * count * QVulkanWindow::MAX_CONCURRENT_FRAME_COUNT;
        if (!createBuffer(bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr,
                          m_uniformBuffer, m_uniformMemory, &m_uniformAllocation)) return false;
        VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, count},
                                      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count * 3}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = count;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        if (m_df->vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool) != VK_SUCCESS) return false;
        std::vector<VkDescriptorSetLayout> layouts(count, m_descriptorLayout);
        std::vector<VkDescriptorSet> sets(count);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = m_descriptorPool;
        allocate.descriptorSetCount = count;
        allocate.pSetLayouts = layouts.data();
        if (m_df->vkAllocateDescriptorSets(m_device, &allocate, sets.data()) != VK_SUCCESS) return false;
        for (uint32_t index = 0; index < count; ++index) {
            DrawRange& range = m_ranges[int(index)];
            range.descriptor = sets[index];
            VkDescriptorBufferInfo buffer{m_uniformBuffer, 0, sizeof(DrawUniform)};
            TextureResource* textures[3] = {&range.base, &range.metalRough, &range.normal};
            VkDescriptorImageInfo images[3]{};
            VkWriteDescriptorSet writes[4]{};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[index], 0, 0,
                         1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &buffer, nullptr};
            for (uint32_t binding = 1; binding < 4; ++binding) {
                const TextureResource& texture = textures[binding - 1]->view ? *textures[binding - 1] : m_whiteTexture;
                images[binding - 1] = {texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[index], binding, 0,
                                   1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[binding - 1], nullptr, nullptr};
            }
            m_df->vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
        }
        return true;
    }

    void destroySingleSampleTargets()
    {
        for (SingleSampleTarget& target : m_singleSampleTargets) {
            if (target.framebuffer) m_df->vkDestroyFramebuffer(m_device, target.framebuffer, nullptr);
            if (target.view) m_df->vkDestroyImageView(m_device, target.view, nullptr);
            if (target.depth) m_df->vkDestroyImage(m_device, target.depth, nullptr);
            if (target.memory) m_df->vkFreeMemory(m_device, target.memory, nullptr);
        }
        m_singleSampleTargets.clear();
        if (m_singleSamplePass) m_df->vkDestroyRenderPass(m_device, m_singleSamplePass, nullptr);
        m_singleSamplePass = VK_NULL_HANDLE;
        updateFramebufferStats();
    }

    void createSingleSampleTargets()
    {
        if (m_window->sampleCountFlagBits() == VK_SAMPLE_COUNT_1_BIT) return;
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = m_window->colorFormat();
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachments[1].format = m_window->depthStencilFormat();
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        subpass.pDepthStencilAttachment = &depth;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 2;
        pass.pAttachments = attachments;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 1;
        pass.pDependencies = &dependency;
        if (m_df->vkCreateRenderPass(m_device, &pass, nullptr, &m_singleSamplePass) != VK_SUCCESS) {
            qWarning() << "Vulkan single-sample render pass unavailable";
            return;
        }
        const VkFormat depthFormat = m_window->depthStencilFormat();
        const bool stencil = depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
                             depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                             depthFormat == VK_FORMAT_D16_UNORM_S8_UINT;
        const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT |
                                                (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        const QSize size = m_window->swapChainImageSize();
        m_singleSampleTargets.resize(size_t(m_window->swapChainImageCount()));
        for (int index = 0; index < m_window->swapChainImageCount(); ++index) {
            SingleSampleTarget& target = m_singleSampleTargets[size_t(index)];
            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format = depthFormat;
            image.extent = {uint32_t(size.width()), uint32_t(size.height()), 1};
            image.mipLevels = image.arrayLayers = 1;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (m_df->vkCreateImage(m_device, &image, nullptr, &target.depth) != VK_SUCCESS) break;
            VkMemoryRequirements requirements{};
            m_df->vkGetImageMemoryRequirements(m_device, target.depth, &requirements);
            const uint32_t type = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = type;
            if (type == UINT32_MAX ||
                m_df->vkAllocateMemory(m_device, &allocation, nullptr, &target.memory) != VK_SUCCESS ||
                m_df->vkBindImageMemory(m_device, target.depth, target.memory, 0) != VK_SUCCESS) break;
            target.allocationSize = requirements.size;
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = target.depth;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = depthFormat;
            view.subresourceRange = {depthAspect, 0, 1, 0, 1};
            if (m_df->vkCreateImageView(m_device, &view, nullptr, &target.view) != VK_SUCCESS) break;
            VkImageView views[2]{m_window->swapChainImageView(index), target.view};
            VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebuffer.renderPass = m_singleSamplePass;
            framebuffer.attachmentCount = 2;
            framebuffer.pAttachments = views;
            framebuffer.width = uint32_t(size.width());
            framebuffer.height = uint32_t(size.height());
            framebuffer.layers = 1;
            if (m_df->vkCreateFramebuffer(m_device, &framebuffer, nullptr, &target.framebuffer) != VK_SUCCESS)
                break;
        }
        for (const SingleSampleTarget& target : m_singleSampleTargets)
            if (!target.framebuffer) {
                qWarning() << "Vulkan single-sample framebuffer unavailable";
                destroySingleSampleTargets();
                break;
            }
        updateFramebufferStats();
    }

    void updateFramebufferStats()
    {
        qint64 bytes = qint64(m_pick.colorAllocation + m_pick.depthAllocation);
        int ownedCount = m_pick.framebuffer ? 1 : 0;
        for (const SingleSampleTarget& target : m_singleSampleTargets)
            if (target.framebuffer) { bytes += qint64(target.allocationSize); ++ownedCount; }
        m_window->setFramebufferStats(bytes, ownedCount);
    }

    void destroyPickTarget()
    {
        if (m_pick.framebuffer) m_df->vkDestroyFramebuffer(m_device, m_pick.framebuffer, nullptr);
        if (m_pick.colorView) m_df->vkDestroyImageView(m_device, m_pick.colorView, nullptr);
        if (m_pick.depthView) m_df->vkDestroyImageView(m_device, m_pick.depthView, nullptr);
        if (m_pick.color) m_df->vkDestroyImage(m_device, m_pick.color, nullptr);
        if (m_pick.depth) m_df->vkDestroyImage(m_device, m_pick.depth, nullptr);
        if (m_pick.colorMemory) m_df->vkFreeMemory(m_device, m_pick.colorMemory, nullptr);
        if (m_pick.depthMemory) m_df->vkFreeMemory(m_device, m_pick.depthMemory, nullptr);
        destroyBuffer(m_pick.readback, m_pick.readbackMemory);
        if (m_pick.pass) m_df->vkDestroyRenderPass(m_device, m_pick.pass, nullptr);
        m_pick = PickTarget();
        updateFramebufferStats();
    }

    bool createPickImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         VkImage& image, VkDeviceMemory& memory, VkImageView& view,
                         VkDeviceSize& allocationSize)
    {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {1, 1, 1};
        imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (m_df->vkCreateImage(m_device, &imageInfo, nullptr, &image) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        m_df->vkGetImageMemoryRequirements(m_device, image, &requirements);
        const uint32_t type = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (m_df->vkAllocateMemory(m_device, &allocation, nullptr, &memory) != VK_SUCCESS ||
            m_df->vkBindImageMemory(m_device, image, memory, 0) != VK_SUCCESS) return false;
        allocationSize = requirements.size;
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
        return m_df->vkCreateImageView(m_device, &viewInfo, nullptr, &view) == VK_SUCCESS;
    }

    void createPickTarget()
    {
        if (!m_pickFragmentShader) return;
        VkFormatProperties formatProperties{};
        m_window->vulkanInstance()->functions()->vkGetPhysicalDeviceFormatProperties(
            m_window->physicalDevice(), VK_FORMAT_R32_UINT, &formatProperties);
        if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            qWarning() << "Vulkan R32_UINT picking attachment unsupported";
            return;
        }
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = VK_FORMAT_R32_UINT;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        attachments[1].format = m_window->depthStencilFormat();
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        subpass.pDepthStencilAttachment = &depth;
        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 2;
        pass.pAttachments = attachments;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 2;
        pass.pDependencies = dependencies;
        if (m_df->vkCreateRenderPass(m_device, &pass, nullptr, &m_pick.pass) != VK_SUCCESS) return;
        const VkFormat depthFormat = m_window->depthStencilFormat();
        const bool stencil = depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
                             depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                             depthFormat == VK_FORMAT_D16_UNORM_S8_UINT;
        const bool imagesReady = createPickImage(VK_FORMAT_R32_UINT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, m_pick.color, m_pick.colorMemory,
            m_pick.colorView, m_pick.colorAllocation) &&
            createPickImage(depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
            m_pick.depth, m_pick.depthMemory, m_pick.depthView, m_pick.depthAllocation);
        if (!imagesReady) { destroyPickTarget(); return; }
        VkImageView views[2]{m_pick.colorView, m_pick.depthView};
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = m_pick.pass;
        framebuffer.attachmentCount = 2;
        framebuffer.pAttachments = views;
        framebuffer.width = framebuffer.height = framebuffer.layers = 1;
        if (m_df->vkCreateFramebuffer(m_device, &framebuffer, nullptr, &m_pick.framebuffer) != VK_SUCCESS ||
            !createBuffer(sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr,
                          m_pick.readback, m_pick.readbackMemory, &m_pick.readbackAllocation)) {
            destroyPickTarget(); return;
        }
        updateFramebufferStats();
    }

    int pickAt(const QPoint& position)
    {
        if (!m_window->scene().hasModel()) return -1;
        if (!m_pick.pipeline || !m_pick.framebuffer || !m_pick.readback ||
            !m_vertexBuffer || !m_indexBuffer || !m_uniformBuffer) return -2;
        const QSize size = m_window->swapChainImageSize();
        if (size.isEmpty()) return -2;
        const int x = std::clamp(int(position.x() * m_window->devicePixelRatio()), 0, size.width() - 1);
        const int y = std::clamp(int(position.y() * m_window->devicePixelRatio()), 0, size.height() - 1);
        QMatrix4x4 pickMatrix;
        pickMatrix(0, 0) = float(size.width());
        pickMatrix(1, 1) = float(size.height());
        pickMatrix(0, 3) = float(size.width() - 2 * x - 1);
        pickMatrix(1, 3) = float(size.height() - 2 * y - 1);
        const RenderFrame frame = makeRenderFrame(m_window->scene(), size);
        const VkDeviceSize frameOffset = m_uniformStride * m_ranges.size() * m_window->currentFrame();
        m_df->vkDeviceWaitIdle(m_device); // 拾取时才等待，避免覆盖尚在使用的动态 UBO。
        void* mapped = nullptr;
        if (m_df->vkMapMemory(m_device, m_uniformMemory, 0, m_uniformAllocation, 0, &mapped) != VK_SUCCESS)
            return -2;
        for (int index = 0; index < m_ranges.size(); ++index) {
            const QMatrix4x4 modelView = frame.view * m_window->subMeshTransform(index);
            const QMatrix4x4 mvp = pickMatrix * m_window->clipCorrectionMatrix() *
                                    frame.projection * modelView;
            DrawUniform uniform{};
            std::memcpy(uniform.mvp, mvp.constData(), sizeof(uniform.mvp));
            std::memcpy(uniform.modelView, modelView.constData(), sizeof(uniform.modelView));
            uniform.options[3] = float(index + 1); // 0 留给背景。
            std::memcpy(static_cast<char*>(mapped) + frameOffset + m_uniformStride * index,
                        &uniform, sizeof(uniform));
        }
        m_df->vkUnmapMemory(m_device, m_uniformMemory);

        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = m_window->graphicsCommandPool();
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        if (m_df->vkAllocateCommandBuffers(m_device, &allocate, &command) != VK_SUCCESS) return -2;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (m_df->vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
            m_df->vkFreeCommandBuffers(m_device, allocate.commandPool, 1, &command);
            return -2;
        }
        VkClearValue clears[2]{};
        clears[0].color.uint32[0] = 0;
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = m_pick.pass;
        pass.framebuffer = m_pick.framebuffer;
        pass.renderArea.extent = {1, 1};
        pass.clearValueCount = 2;
        pass.pClearValues = clears;
        m_df->vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {1, 1}};
        const VkDeviceSize offset = 0;
        m_df->vkCmdSetViewport(command, 0, 1, &viewport);
        m_df->vkCmdSetScissor(command, 0, 1, &scissor);
        m_df->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pick.pipeline);
        m_df->vkCmdBindVertexBuffers(command, 0, 1, &m_vertexBuffer, &offset);
        m_df->vkCmdBindIndexBuffer(command, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        const PushConstants constants{};
        m_df->vkCmdPushConstants(command, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
        for (int index = 0; index < m_ranges.size(); ++index) {
            const DrawRange& range = m_ranges[index];
            if (!range.indexCount || !range.descriptor || !m_window->subMeshVisible(index)) continue;
            const uint32_t dynamicOffset = uint32_t(frameOffset + m_uniformStride * index);
            m_df->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                          0, 1, &range.descriptor, 1, &dynamicOffset);
            m_df->vkCmdDrawIndexed(command, range.indexCount, 1, range.firstIndex, range.vertexOffset, 0);
        }
        m_df->vkCmdEndRenderPass(command);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {1, 1, 1};
        m_df->vkCmdCopyImageToBuffer(command, m_pick.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     m_pick.readback, 1, &copy);
        VkBufferMemoryBarrier readBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        readBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        readBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        readBarrier.srcQueueFamilyIndex = readBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readBarrier.buffer = m_pick.readback;
        readBarrier.size = sizeof(uint32_t);
        m_df->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                   0, 0, nullptr, 1, &readBarrier, 0, nullptr);
        const VkResult endResult = m_df->vkEndCommandBuffer(command);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        const VkResult submitResult = endResult == VK_SUCCESS
            ? m_df->vkQueueSubmit(m_window->graphicsQueue(), 1, &submit, VK_NULL_HANDLE) : endResult;
        const VkResult waitResult = submitResult == VK_SUCCESS
            ? m_df->vkQueueWaitIdle(m_window->graphicsQueue()) : submitResult;
        m_df->vkFreeCommandBuffers(m_device, allocate.commandPool, 1, &command);
        if (waitResult != VK_SUCCESS) return -2;
        mapped = nullptr;
        if (m_df->vkMapMemory(m_device, m_pick.readbackMemory, 0, sizeof(uint32_t), 0, &mapped) != VK_SUCCESS)
            return -2;
        uint32_t id = 0;
        std::memcpy(&id, mapped, sizeof(id));
        m_df->vkUnmapMemory(m_device, m_pick.readbackMemory);
        return id && id <= uint32_t(m_ranges.size()) ? int(id - 1) : -1;
    }

    void createModelBuffers()
    {
        if (!m_df || m_uploadedRevision == m_window->modelRevision()) return;
        m_df->vkDeviceWaitIdle(m_device);
        destroyModelDescriptors();
        destroyTextures();
        destroyTexture(m_whiteTexture);
        m_whiteTexture.source = QImage();
        destroyBuffer(m_vertexBuffer, m_vertexMemory);
        destroyBuffer(m_indexBuffer, m_indexMemory);
        destroyBuffer(m_gridBuffer, m_gridMemory);
        m_gridVertexCount = 0;
        m_gridAllocation = 0;
        m_vertexAllocation = m_indexAllocation = 0;
        m_ranges.clear();
        m_uploadedIndexCount = 0;
        m_uploadedVertexCount = 0;
        if (!m_window->scene().hasModel()) {
            m_uploadedRevision = m_window->modelRevision();
            m_uploadedGridRevision = m_window->gridRevision();
            m_window->setResourceStats(m_overlayAllocation + m_pick.readbackAllocation, 0, 0, 0, 0,
                                       int(bool(m_overlayBuffer)) + int(bool(m_pick.readback)));
            return;
        }
        const Mesh& mesh = m_window->scene().mesh();
        rebuildGrid();
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const SubMesh& subMesh : mesh.subMeshes) {
            DrawRange range;
            range.firstIndex = uint32_t(indices.size());
            range.vertexOffset = int32_t(vertices.size());
            range.indexCount = uint32_t(subMesh.indices.size());
            range.color = subMesh.diffuseColor;
            range.metallic = subMesh.metallicFactor;
            range.roughness = subMesh.roughnessFactor;
            range.base.source = loadTextureImage(subMesh.textureData, subMesh.texturePath,
                                                 subMesh.flipTextureVertically);
            range.metalRough.source = loadTextureImage(subMesh.metallicRoughnessData,
                subMesh.metallicRoughnessPath, subMesh.flipTextureVertically);
            range.normal.source = loadTextureImage(subMesh.normalData, subMesh.normalPath,
                                                   subMesh.flipTextureVertically);
            vertices.insert(vertices.end(), subMesh.vertices.begin(), subMesh.vertices.end());
            for (unsigned int index : subMesh.indices) indices.push_back(index);
            m_ranges.push_back(range);
        }
        if (vertices.empty() || indices.empty() ||
            !createBuffer(vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          vertices.data(), m_vertexBuffer, m_vertexMemory, &m_vertexAllocation) ||
            !createBuffer(indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          indices.data(), m_indexBuffer, m_indexMemory, &m_indexAllocation)) {
            destroyBuffer(m_vertexBuffer, m_vertexMemory);
            destroyBuffer(m_indexBuffer, m_indexMemory);
            m_ranges.clear();
            return;
        }
        if (!m_whiteTexture.view) {
            m_whiteTexture.source = QImage(1, 1, QImage::Format_RGBA8888);
            m_whiteTexture.source.fill(Qt::white);
            if (!createTexture(m_whiteTexture)) {
                qWarning() << "Vulkan fallback texture creation failed";
                return;
            }
        }
        rebuildTextures();
        m_uploadedRevision = m_window->modelRevision();
        m_uploadedVertexCount = int(vertices.size());
        m_uploadedIndexCount = int(indices.size());
        m_reportedDrawFailure = false;
        qInfo() << "Vulkan model buffers uploaded: vertices=" << m_uploadedVertexCount
                << "indices=" << m_uploadedIndexCount << "ranges=" << m_ranges.size();
        // Vulkan 缓冲已经建立，释放 CPU 顶点和索引；场景元数据仍然保留。
        m_window->scene().mesh().releaseGeometry();
        m_window->modelUploadFinished();
    }

    void rebuildTextures()
    {
        m_df->vkDeviceWaitIdle(m_device);
        destroyModelDescriptors();
        destroyTextures();
        bool uploaded = true;
        for (DrawRange& range : m_ranges)
            for (TextureResource* texture : {&range.base, &range.metalRough, &range.normal})
                if (!texture->source.isNull() && !createTexture(*texture)) uploaded = false;
        if (!uploaded) qWarning() << "One or more Vulkan textures could not be uploaded";
        if (!createModelDescriptors()) qWarning() << "Vulkan model descriptors unavailable";
        m_uploadedTextureRevision = m_window->textureRevision();
        qint64 gpu = m_whiteTexture.allocationSize, cpu = 0;
        int count = m_whiteTexture.view ? 1 : 0;
        for (const DrawRange& range : m_ranges)
            for (const TextureResource* texture : {&range.base, &range.metalRough, &range.normal}) {
                if (texture->view) { gpu += texture->allocationSize; ++count; }
                if (!texture->source.isNull()) cpu += qint64(texture->source.bytesPerLine()) * texture->source.height();
            }
        m_window->setResourceStats(m_vertexAllocation + m_uniformAllocation + m_overlayAllocation + m_gridAllocation +
                                   m_pick.readbackAllocation, m_indexAllocation, gpu, cpu, count,
                                   int(bool(m_vertexBuffer)) + int(bool(m_uniformBuffer)) +
                                   int(bool(m_overlayBuffer)) + int(bool(m_gridBuffer)) + int(bool(m_pick.readback)));
    }

    VkShaderModule createShaderModule(const QByteArray& bytes)
    {
        if (bytes.isEmpty() || bytes.size() % 4) return VK_NULL_HANDLE;
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = size_t(bytes.size());
        info.pCode = reinterpret_cast<const uint32_t*>(bytes.constData());
        VkShaderModule module = VK_NULL_HANDLE;
        return m_df->vkCreateShaderModule(m_device, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
    }

    void createPipeline(bool singleSample = false, bool picking = false)
    {
        if (!m_vertexShader || !(picking ? m_pickFragmentShader : m_fragmentShader) || !m_descriptorLayout) {
            qWarning() << "Vulkan pipeline skipped: shader module unavailable";
            return;
        }
        if (!m_pipelineLayout) {
            VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
            layout.setLayoutCount = 1; layout.pSetLayouts = &m_descriptorLayout;
            const VkResult layoutResult = m_df->vkCreatePipelineLayout(m_device, &layout, nullptr, &m_pipelineLayout);
            if (layoutResult != VK_SUCCESS) {
                qWarning() << "vkCreatePipelineLayout failed:" << layoutResult;
                return;
            }
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, m_vertexShader, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                     picking ? m_pickFragmentShader : m_fragmentShader, "main", nullptr};
        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[3]{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                         {2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 3; input.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = 1; viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = picking ? VK_POLYGON_MODE_FILL : m_window->scene().options().wireframe && m_wireframeSupported ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL; raster.cullMode = picking ? VK_CULL_MODE_NONE : m_window->scene().options().faceCulling ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples = singleSample || picking ? VK_SAMPLE_COUNT_1_BIT : m_window->sampleCountFlagBits();
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; depth.depthTestEnable = picking || m_window->scene().options().depthTest ? VK_TRUE : VK_FALSE; depth.depthWriteEnable = depth.depthTestEnable; depth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &attachment;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline.stageCount = 2; pipeline.pStages = stages; pipeline.pVertexInputState = &input; pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &multisample; pipeline.pDepthStencilState = &depth; pipeline.pColorBlendState = &blend; pipeline.pDynamicState = &dynamic; pipeline.layout = m_pipelineLayout; pipeline.renderPass = picking ? m_pick.pass : singleSample ? m_singleSamplePass : m_window->defaultRenderPass();
        VkPipeline& mainPipeline = picking ? m_pick.pipeline : singleSample ? m_singleSamplePipeline : m_pipeline;
        VkPipeline& outlinePipeline = singleSample ? m_singleSampleOutlinePipeline : m_outlinePipeline;
        VkPipeline& selectionPipeline = singleSample ? m_singleSampleSelectionPipeline : m_selectionPipeline;
        const VkResult pipelineResult = m_df->vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &mainPipeline);
        if (pipelineResult != VK_SUCCESS) {
            qWarning() << "vkCreateGraphicsPipelines failed:" << pipelineResult
                       << "renderPass=" << bool(pipeline.renderPass)
                       << "samples=" << int(multisample.rasterizationSamples);
            if (!m_pipeline) {
                m_df->vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
                m_pipelineLayout = VK_NULL_HANDLE;
            }
            return;
        }
        if (picking) return;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_FRONT_BIT;
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        if (m_df->vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline,
                                             nullptr, &outlinePipeline) != VK_SUCCESS)
            qWarning() << "Vulkan selection outline pipeline failed";
        raster.cullMode = VK_CULL_MODE_NONE;
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        if (m_df->vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline,
                                             nullptr, &selectionPipeline) != VK_SUCCESS)
            qWarning() << "Vulkan selection overlay pipeline failed";
    }

    void createOverlayPipeline(bool singleSample = false, bool grid = false)
    {
        if (!m_overlayVertexShader || !m_overlayFragmentShader) return;
        if (!m_overlayPipelineLayout) {
            VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &push;
            if (m_df->vkCreatePipelineLayout(m_device, &layout, nullptr, &m_overlayPipelineLayout) != VK_SUCCESS)
                return;
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_VERTEX_BIT, m_overlayVertexShader, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_FRAGMENT_BIT, m_overlayFragmentShader, "main", nullptr};
        VkVertexInputBindingDescription binding{0, sizeof(OverlayVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[2]{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 2;
        input.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = singleSample ? VK_SAMPLE_COUNT_1_BIT : m_window->sampleCountFlagBits();
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = grid ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp = grid ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        VkDynamicState states[2]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline.stageCount = 2;
        pipeline.pStages = stages;
        pipeline.pVertexInputState = &input;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState = &viewport;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState = &multisample;
        pipeline.pDepthStencilState = &depth;
        pipeline.pColorBlendState = &blend;
        pipeline.pDynamicState = &dynamic;
        pipeline.layout = m_overlayPipelineLayout;
        pipeline.renderPass = singleSample ? m_singleSamplePass : m_window->defaultRenderPass();
        VkPipeline& overlayPipeline = grid
            ? (singleSample ? m_singleSampleGridPipeline : m_gridPipeline)
            : (singleSample ? m_singleSampleOverlayPipeline : m_overlayPipeline);
        if (m_df->vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline,
                                             nullptr, &overlayPipeline) != VK_SUCCESS)
            qWarning() << "Vulkan overlay pipeline creation failed";
    }

    void destroyPipeline()
    {
        if (m_pick.pipeline) m_df->vkDestroyPipeline(m_device, m_pick.pipeline, nullptr);
        m_pick.pipeline = VK_NULL_HANDLE;
        if (m_pipeline) m_df->vkDestroyPipeline(m_device, m_pipeline, nullptr);
        if (m_outlinePipeline) m_df->vkDestroyPipeline(m_device, m_outlinePipeline, nullptr);
        if (m_selectionPipeline) m_df->vkDestroyPipeline(m_device, m_selectionPipeline, nullptr);
        if (m_overlayPipeline) m_df->vkDestroyPipeline(m_device, m_overlayPipeline, nullptr);
        if (m_gridPipeline) m_df->vkDestroyPipeline(m_device, m_gridPipeline, nullptr);
        if (m_singleSamplePipeline) m_df->vkDestroyPipeline(m_device, m_singleSamplePipeline, nullptr);
        if (m_singleSampleOutlinePipeline) m_df->vkDestroyPipeline(m_device, m_singleSampleOutlinePipeline, nullptr);
        if (m_singleSampleSelectionPipeline) m_df->vkDestroyPipeline(m_device, m_singleSampleSelectionPipeline, nullptr);
        if (m_singleSampleOverlayPipeline) m_df->vkDestroyPipeline(m_device, m_singleSampleOverlayPipeline, nullptr);
        if (m_singleSampleGridPipeline) m_df->vkDestroyPipeline(m_device, m_singleSampleGridPipeline, nullptr);
        if (m_pipelineLayout) m_df->vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        if (m_overlayPipelineLayout) m_df->vkDestroyPipelineLayout(m_device, m_overlayPipelineLayout, nullptr);
        m_pipeline = m_outlinePipeline = m_selectionPipeline = VK_NULL_HANDLE;
        m_pipelineLayout = VK_NULL_HANDLE;
        m_overlayPipeline = VK_NULL_HANDLE;
        m_gridPipeline = VK_NULL_HANDLE;
        m_singleSamplePipeline = m_singleSampleOutlinePipeline = m_singleSampleSelectionPipeline = VK_NULL_HANDLE;
        m_singleSampleOverlayPipeline = VK_NULL_HANDLE;
        m_singleSampleGridPipeline = VK_NULL_HANDLE;
        m_overlayPipelineLayout = VK_NULL_HANDLE;
    }

    VulkanWindow* m_window = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_df = nullptr;
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE, m_indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexMemory = VK_NULL_HANDLE, m_indexMemory = VK_NULL_HANDLE;
    VkShaderModule m_vertexShader = VK_NULL_HANDLE, m_fragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_pickFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_overlayVertexShader = VK_NULL_HANDLE, m_overlayFragmentShader = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_outlinePipeline = VK_NULL_HANDLE;
    VkPipeline m_selectionPipeline = VK_NULL_HANDLE;
    VkPipeline m_overlayPipeline = VK_NULL_HANDLE;
    VkPipeline m_gridPipeline = VK_NULL_HANDLE;
    VkPipeline m_singleSamplePipeline = VK_NULL_HANDLE;
    VkPipeline m_singleSampleOutlinePipeline = VK_NULL_HANDLE;
    VkPipeline m_singleSampleSelectionPipeline = VK_NULL_HANDLE;
    VkPipeline m_singleSampleOverlayPipeline = VK_NULL_HANDLE;
    VkPipeline m_singleSampleGridPipeline = VK_NULL_HANDLE;
    VkRenderPass m_singleSamplePass = VK_NULL_HANDLE;
    std::vector<SingleSampleTarget> m_singleSampleTargets;
    PickTarget m_pick;
    VkPipelineLayout m_overlayPipelineLayout = VK_NULL_HANDLE;
    VkBuffer m_overlayBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_overlayMemory = VK_NULL_HANDLE;
    VkBuffer m_gridBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_gridMemory = VK_NULL_HANDLE;
    uint32_t m_gridVertexCount = 0;
    VkDeviceSize m_gridAllocation = 0;
    uint64_t m_uploadedGridRevision = UINT64_MAX;
    uint32_t m_arrowVertexCount = 0, m_ringVertexCount = 0, m_scaleVertexCount = 0;
    VkDeviceSize m_overlayAllocation = 0;
    uint64_t m_pipelineRevision = 0;
    bool m_wireframeSupported = false;
    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkBuffer m_uniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_uniformMemory = VK_NULL_HANDLE;
    VkDeviceSize m_uniformStride = 0, m_vertexAllocation = 0, m_indexAllocation = 0;
    VkDeviceSize m_uniformAllocation = 0;
    TextureResource m_whiteTexture;
    uint64_t m_uploadedTextureRevision = 0;
    QVector<DrawRange> m_ranges;
    uint64_t m_uploadedRevision = 0;
    int m_uploadedIndexCount = 0;
    int m_uploadedVertexCount = 0;
    bool m_reportedDrawFailure = false;
};

}

VulkanWindow::VulkanWindow(QVulkanInstance* instance, QWindow* parent) : QVulkanWindow(parent)
{
    setVulkanInstance(instance);
    m_ready = instance && instance->isValid();
    if (m_ready) {
        const QList<int> counts = supportedSampleCounts();
        const int samples = counts.contains(4) ? 4 : counts.contains(2) ? 2 : 1;
        m_multisamplingAvailable = samples > 1;
        m_scene.options().antialiasing = m_multisamplingAvailable;
        if (m_multisamplingAvailable) setSampleCount(samples);
    }
    m_fpsTimer.start();
    m_frameTimer.start();
    m_renderTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_renderTimer, &QTimer::timeout, this, [this]() { requestUpdate(); });
}

VulkanWindow::~VulkanWindow()
{
    if (m_loadThread) {
        m_loadThread->quit();
        m_loadThread->wait();
        delete m_loadWorker;
        m_loadWorker = nullptr;
        delete m_loadThread;
    }
}

bool VulkanWindow::loadModel(const QString& path, QString* error)
{
    Mesh mesh;
    if (!MeshLoader::load(path, mesh, error)) return false;
    setLoadedMesh(std::move(mesh), path);
    return true;
}

bool VulkanWindow::loadModelAsync(const QString& path)
{
    if (m_loading) return false;
    if (!m_loadThread) {
        qRegisterMetaType<Mesh>("Mesh");
        m_loadThread = new QThread(this);
        m_loadWorker = new ModelLoaderWorker;
        m_loadWorker->moveToThread(m_loadThread);
        connect(m_loadWorker, &ModelLoaderWorker::progress, this, [this](int percent) {
            if (m_loading) emit progressChanged(std::min(90, percent * 9 / 10));
        });
        connect(m_loadWorker, &ModelLoaderWorker::finished, this,
                [this](bool ok, const QString& error, const Mesh& mesh) {
            if (!m_loading) return;
            if (!ok) {
                m_loading = false;
                emit loadFailed(error.isEmpty() ? QStringLiteral("加载失败") : error);
                emit loadFinished();
                return;
            }
            setLoadedMesh(mesh, m_currentPath);
            emit progressChanged(90);
        });
        m_loadThread->start();
    }
    m_currentPath = path;
    m_loading = true;
    emit loadStarted();
    QMetaObject::invokeMethod(m_loadWorker, "load", Qt::QueuedConnection, Q_ARG(QString, path));
    return true;
}

void VulkanWindow::modelUploadFinished()
{
    if (!m_loading) return;
    m_loading = false;
    emit progressChanged(100);
    emit loadFinished();
}

void VulkanWindow::setLoadedMesh(Mesh mesh, const QString& path)
{
    m_scene.setMesh(std::move(mesh), path);
    m_bounds.clear();
    m_visible.assign(m_scene.mesh().subMeshes.size(), true);
    m_transforms.assign(m_scene.mesh().subMeshes.size(), TransformState());
    for (const SubMesh& subMesh : m_scene.mesh().subMeshes) {
        Bounds bounds{QVector3D(FLT_MAX, FLT_MAX, FLT_MAX), QVector3D(-FLT_MAX, -FLT_MAX, -FLT_MAX)};
        for (const Vertex& vertex : subMesh.vertices) {
            const QVector3D point(vertex.px, vertex.py, vertex.pz);
            bounds.min.setX(std::min(bounds.min.x(), point.x()));
            bounds.min.setY(std::min(bounds.min.y(), point.y()));
            bounds.min.setZ(std::min(bounds.min.z(), point.z()));
            bounds.max.setX(std::max(bounds.max.x(), point.x()));
            bounds.max.setY(std::max(bounds.max.y(), point.y()));
            bounds.max.setZ(std::max(bounds.max.z(), point.z()));
        }
        m_bounds.push_back(bounds);
    }
    m_undo.clear();
    m_redo.clear();
    m_pendingPick = QPoint(-1, -1);
    m_scene.options().outlineWidth = m_scene.mesh().radius() * 0.005f;
    emit selectionChanged(false);
    emit historyChanged(false, false);
    ++m_modelRevision;
    emit modelLoaded(QString("%1  顶点%2  三角%3  子网格%4")
        .arg(QFileInfo(path).fileName()).arg(m_scene.mesh().vertexCount())
        .arg(m_scene.mesh().triangleCount()).arg(subMeshCount()));
    requestUpdate();
}

void VulkanWindow::clearModel()
{
    m_scene.clear();
    m_bounds.clear();
    m_visible.clear();
    m_transforms.clear();
    m_undo.clear();
    m_redo.clear();
    m_pendingPick = QPoint(-1, -1);
    emit selectionChanged(false);
    emit historyChanged(false, false);
    ++m_modelRevision;
    requestUpdate();
}
QVulkanWindowRenderer* VulkanWindow::createRenderer() { return new VulkanWindowRenderer(this); }

#endif

#include "vulkanwindow.h"

#if defined(GL3D_HAS_VULKAN)

#include "renderframe.h"
#include <QVulkanDeviceFunctions>
#include <QVulkanInstance>
#include <QCoreApplication>
#include <QFile>
#include <QVector>
#include <cstring>
#include <vector>

namespace {

struct DrawRange { uint32_t firstIndex = 0; uint32_t indexCount = 0; int32_t vertexOffset = 0; };
struct PushConstants { QMatrix4x4 mvp; };

static QByteArray readBinary(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

class VulkanWindowRenderer final : public QVulkanWindowRenderer {
public:
    explicit VulkanWindowRenderer(VulkanWindow* window) : m_window(window) {}

    void initResources() override
    {
        m_device = m_window->device();
        m_df = m_window->vulkanInstance()->deviceFunctions(m_device);
        const QString dir = QCoreApplication::applicationDirPath() + "/shaders/";
        m_vertexShader = createShaderModule(readBinary(dir + "vulkan_model.vert.spv"));
        m_fragmentShader = createShaderModule(readBinary(dir + "vulkan_model.frag.spv"));
    }

    void initSwapChainResources() override { createPipeline(); }

    void releaseSwapChainResources() override { destroyPipeline(); }

    void releaseResources() override
    {
        if (!m_df) return;
        m_df->vkDeviceWaitIdle(m_device);
        destroyPipeline();
        if (m_vertexShader) m_df->vkDestroyShaderModule(m_device, m_vertexShader, nullptr);
        if (m_fragmentShader) m_df->vkDestroyShaderModule(m_device, m_fragmentShader, nullptr);
        destroyBuffer(m_vertexBuffer, m_vertexMemory);
        destroyBuffer(m_indexBuffer, m_indexMemory);
        m_vertexShader = VK_NULL_HANDLE;
        m_fragmentShader = VK_NULL_HANDLE;
        m_df = nullptr;
    }

    void startNextFrame() override
    {
        createModelBuffers();
        if (m_pipeline && m_indexBuffer && m_window->scene().hasModel()) {
            const RenderFrame frame = makeRenderFrame(m_window->scene(), m_window->swapChainImageSize());
            PushConstants constants{m_window->clipCorrectionMatrix() * frame.projection * frame.view * frame.model};
            VkCommandBuffer cmd = m_window->currentCommandBuffer();
            const QSize size = m_window->swapChainImageSize();
            VkViewport viewport{0.0f, 0.0f, float(size.width()), float(size.height()), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {uint32_t(size.width()), uint32_t(size.height())}};
            const VkDeviceSize offset = 0;
            m_df->vkCmdSetViewport(cmd, 0, 1, &viewport);
            m_df->vkCmdSetScissor(cmd, 0, 1, &scissor);
            m_df->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
            m_df->vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer, &offset);
            m_df->vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            m_df->vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                     0, sizeof(constants), &constants);
            for (const DrawRange& range : m_ranges)
                m_df->vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, range.vertexOffset, 0);
        }
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
                      VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (m_df->vkCreateBuffer(m_device, &info, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        m_df->vkGetBufferMemoryRequirements(m_device, buffer, &requirements);
        const uint32_t type = findMemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) { destroyBuffer(buffer, memory); return false; }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
        if (m_df->vkAllocateMemory(m_device, &allocate, nullptr, &memory) != VK_SUCCESS ||
            m_df->vkBindBufferMemory(m_device, buffer, memory, 0) != VK_SUCCESS) {
            destroyBuffer(buffer, memory); return false;
        }
        if (data) {
            void* mapped = nullptr;
            if (m_df->vkMapMemory(m_device, memory, 0, size, 0, &mapped) != VK_SUCCESS) {
                destroyBuffer(buffer, memory); return false;
            }
            std::memcpy(mapped, data, size_t(size));
            m_df->vkUnmapMemory(m_device, memory);
        }
        return true;
    }

    void createModelBuffers()
    {
        if (!m_df || !m_window->scene().hasModel()) return;
        const Mesh& mesh = m_window->scene().mesh();
        if (m_uploadedPath == m_window->scene().sourcePath() && m_uploadedIndexCount == mesh.triangleCount() * 3)
            return;
        m_df->vkDeviceWaitIdle(m_device);
        destroyBuffer(m_vertexBuffer, m_vertexMemory);
        destroyBuffer(m_indexBuffer, m_indexMemory);
        m_ranges.clear();
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const SubMesh& subMesh : mesh.subMeshes) {
            DrawRange range;
            range.firstIndex = uint32_t(indices.size());
            range.vertexOffset = int32_t(vertices.size());
            range.indexCount = uint32_t(subMesh.indices.size());
            vertices.insert(vertices.end(), subMesh.vertices.begin(), subMesh.vertices.end());
            for (unsigned int index : subMesh.indices) indices.push_back(index);
            if (range.indexCount) m_ranges.push_back(range);
        }
        if (vertices.empty() || indices.empty() ||
            !createBuffer(vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          vertices.data(), m_vertexBuffer, m_vertexMemory) ||
            !createBuffer(indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          indices.data(), m_indexBuffer, m_indexMemory)) {
            destroyBuffer(m_vertexBuffer, m_vertexMemory);
            destroyBuffer(m_indexBuffer, m_indexMemory);
            m_ranges.clear();
            return;
        }
        m_uploadedPath = m_window->scene().sourcePath();
        m_uploadedIndexCount = int(indices.size());
        // Vulkan 缓冲已经建立，释放 CPU 顶点和索引；场景元数据仍然保留。
        m_window->scene().mesh().releaseGeometry();
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

    void createPipeline()
    {
        if (!m_vertexShader || !m_fragmentShader) return;
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        if (m_df->vkCreatePipelineLayout(m_device, &layout, nullptr, &m_pipelineLayout) != VK_SUCCESS) return;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, m_vertexShader, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, m_fragmentShader, "main", nullptr};
        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[2]{{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 2; input.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = 1; viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples = m_window->sampleCountFlagBits();
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; depth.depthTestEnable = VK_TRUE; depth.depthWriteEnable = VK_TRUE; depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &attachment;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline.stageCount = 2; pipeline.pStages = stages; pipeline.pVertexInputState = &input; pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &multisample; pipeline.pDepthStencilState = &depth; pipeline.pColorBlendState = &blend; pipeline.pDynamicState = &dynamic; pipeline.layout = m_pipelineLayout; pipeline.renderPass = m_window->defaultRenderPass();
        if (m_df->vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_pipeline) != VK_SUCCESS) {
            m_df->vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE;
        }
    }

    void destroyPipeline()
    {
        if (m_pipeline) m_df->vkDestroyPipeline(m_device, m_pipeline, nullptr);
        if (m_pipelineLayout) m_df->vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipeline = VK_NULL_HANDLE; m_pipelineLayout = VK_NULL_HANDLE;
    }

    VulkanWindow* m_window = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_df = nullptr;
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE, m_indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexMemory = VK_NULL_HANDLE, m_indexMemory = VK_NULL_HANDLE;
    VkShaderModule m_vertexShader = VK_NULL_HANDLE, m_fragmentShader = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    QVector<DrawRange> m_ranges;
    QString m_uploadedPath;
    int m_uploadedIndexCount = 0;
};

}

VulkanWindow::VulkanWindow(QVulkanInstance* instance, QWindow* parent) : QVulkanWindow(parent)
{
    setVulkanInstance(instance);
    m_ready = instance && instance->isValid();
}

VulkanWindow::~VulkanWindow() = default;

bool VulkanWindow::loadModel(const QString& path, QString* error) { return m_scene.loadModel(path, error); }
void VulkanWindow::clearModel() { m_scene.clear(); }
QVulkanWindowRenderer* VulkanWindow::createRenderer() { return new VulkanWindowRenderer(this); }

#endif

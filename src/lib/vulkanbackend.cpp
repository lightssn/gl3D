#include "vulkanbackend.h"

#if defined(GL3D_HAS_VULKAN)

VulkanBackend::~VulkanBackend()
{
    shutdown();
}

bool VulkanBackend::initialize(const char* applicationName)
{
    if (isInitialized())
        return true;

    m_error.clear();

    VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    applicationInfo.pApplicationName = applicationName ? applicationName : "gl3d";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    applicationInfo.pEngineName = "gl3d";
    applicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &applicationInfo;

    const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
    if (result != VK_SUCCESS) {
        m_instance = VK_NULL_HANDLE;
        m_error = QStringLiteral("vkCreateInstance failed: ")
            + QString::number(static_cast<int>(result));
        return false;
    }

    return true;
}

void VulkanBackend::shutdown()
{
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

#endif

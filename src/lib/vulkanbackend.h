#pragma once

#if defined(GL3D_HAS_VULKAN)

#include <QString>
#include <vulkan/vulkan.h>

// Qt-independent Vulkan bootstrap. Window-system surface creation belongs to
// the platform/backend layer and is deliberately not mixed into this class.
class VulkanBackend final {
public:
    VulkanBackend() = default;
    ~VulkanBackend();

    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    bool initialize(const char* applicationName = "gl3d");
    void shutdown();

    bool isInitialized() const { return m_instance != VK_NULL_HANDLE; }
    VkInstance instance() const { return m_instance; }
    const QString& errorString() const { return m_error; }

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    QString m_error;
};

#endif

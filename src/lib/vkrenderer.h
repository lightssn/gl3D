#pragma once
#if defined(GL3D_HAS_VULKAN)
#include "gl3d_export.h"
#include "renderer.h"
#include <QVulkanWindow>
#include <memory>

class VKWindow;

// GPU resources follow Qt's device and swapchain callbacks.
class GL3D_EXPORT VKRenderer final : public QVulkanWindowRenderer, public Renderer {
public:
    explicit VKRenderer(VKWindow* window);
    ~VKRenderer() override;
    void initResources() override;
    void initSwapChainResources() override;
    void releaseSwapChainResources() override;
    void releaseResources() override;
    void startNextFrame() override;

private:
    class State;
    std::unique_ptr<State> m_state;
};
#endif

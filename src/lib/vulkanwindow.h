#pragma once

#if defined(GL3D_HAS_VULKAN)

#include "gl3d_export.h"
#include "renderscene.h"
#include "renderview.h"
#include <QVulkanWindow>

class QVulkanInstance;

// Vulkan 窗口第一阶段：Qt 管理 surface、swapchain 和同步对象。
// 模型管线将在此窗口稳定后接入，--vulkan 用于验证真实 Vulkan 帧循环。
class GL3D_EXPORT VulkanWindow final : public QVulkanWindow, public RenderView {
    Q_OBJECT

public:
    explicit VulkanWindow(QVulkanInstance* instance, QWindow* parent = nullptr);
    ~VulkanWindow() override;

    bool isReady() const { return m_ready; }
    bool loadModel(const QString& path, QString* error = nullptr);
    void clearModel();
    const RenderScene& scene() const { return m_scene; }
    RenderScene& scene() { return m_scene; }
    RenderBackend backend() const override { return RenderBackend::Vulkan; }

protected:
    QVulkanWindowRenderer* createRenderer() override;

private:
    bool m_ready = false;
    RenderScene m_scene;
};

#endif

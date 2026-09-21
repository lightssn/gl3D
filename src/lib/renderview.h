#pragma once

#include "renderscene.h"

enum class RenderBackend {
    OpenGL,
    Vulkan
};

// 与窗口类型无关的渲染视图接口。
// QOpenGLWindow 和 QVulkanWindow 分别实现底层提交，但共享场景入口。
class RenderView {
public:
    virtual ~RenderView() = default;
    virtual RenderBackend backend() const = 0;
    virtual RenderScene& scene() = 0;
    virtual const RenderScene& scene() const = 0;
};


#pragma once

#include "renderscene.h"
#include "debugstats.h"

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
    virtual DebugSnapshot debugSnapshot() = 0;
    virtual void resetView() = 0;
    virtual void setOrtho(bool enabled) = 0;
    virtual void setWireframe(bool enabled) = 0;
    virtual void setAntialiasing(bool enabled) = 0;
    virtual bool supportsAntialiasing() const { return true; }
    virtual void setMipmaps(bool enabled) = 0;
    virtual void setDepthTest(bool enabled) = 0;
    virtual void setFaceCulling(bool enabled) = 0;
    virtual void setPbr(bool enabled) = 0;
    virtual void setNormalMap(bool enabled) = 0;
    virtual void setFixedFpsEnabled(bool enabled) = 0;
    virtual void setTargetFps(int fps) = 0;
    virtual void setDebugView(int mode) = 0;
    virtual float outlineWidth() const = 0;
    virtual void setOutlineWidth(float width) = 0;
    virtual int subMeshCount() const = 0;
    virtual QString subMeshName(int index) const = 0;
    virtual bool subMeshVisible(int index) const = 0;
    virtual void setSubMeshVisible(int index, bool visible) = 0;
    virtual int selectedSubMesh() const = 0;
    virtual void selectSubMesh(int index) = 0;
    virtual void setTransformMode(int mode) = 0;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual void setClearColor(float red, float green, float blue) = 0;
};

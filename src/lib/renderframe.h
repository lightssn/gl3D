#pragma once

#include "renderscene.h"
#include <QMatrix4x4>
#include <QSize>

// 一帧的后端无关输入。矩阵、选项和场景数据由顶层逻辑准备，
// OpenGL/Vulkan 只负责把它转换为各自 API 的资源和命令。
struct RenderFrame {
    QMatrix4x4 view;
    QMatrix4x4 projection;
    QMatrix4x4 model;
    const RenderScene* scene = nullptr;
};

inline RenderFrame makeRenderFrame(const RenderScene& scene, const QSize& size,
                                   const QMatrix4x4& model = QMatrix4x4())
{
    RenderFrame frame;
    frame.scene = &scene;
    frame.view = scene.cameraController().camera().viewMatrix();
    const float aspect = size.height() > 0
        ? static_cast<float>(size.width()) / static_cast<float>(size.height()) : 1.0f;
    frame.projection = scene.cameraController().camera().projMatrix(aspect);
    frame.model = model;
    return frame;
}


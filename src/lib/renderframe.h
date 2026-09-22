#pragma once

#include "renderscene.h"
#include <QMatrix4x4>
#include <QSize>
#include <QVector4D>

// 两种后端共用的相机矩阵，由场景和当前视口尺寸生成。
struct RenderFrame {
    QMatrix4x4 view;
    QMatrix4x4 projection;
};

inline RenderFrame makeRenderFrame(const RenderScene& scene, const QSize& size)
{
    RenderFrame frame;
    frame.view = scene.cameraController().camera().viewMatrix();
    const float aspect = size.height() > 0
        ? static_cast<float>(size.width()) / static_cast<float>(size.height()) : 1.0f;
    frame.projection = scene.cameraController().camera().projMatrix(aspect);
    return frame;
}

// 两种后端共用坐标轴 HUD 的视角和视口比例计算。
inline QMatrix4x4 makeAxesHudMatrix(const QMatrix4x4& view, const QSize& viewportSize)
{
    QMatrix4x4 viewRotation = view;
    viewRotation.setColumn(3, QVector4D(0, 0, 0, 1));
    QMatrix4x4 projection;
    projection.ortho(-1, 1, -1, 1, -100, 100);
    const float aspect = viewportSize.height() > 0
        ? float(viewportSize.width()) / float(viewportSize.height()) : 1.0f;
    QMatrix4x4 corner;
    corner.translate(-0.8f, -0.8f);
    corner.scale(0.2f / aspect, 0.2f);
    return corner * projection * viewRotation;
}

#pragma once

#include "gl3d_export.h"
#include <QMatrix4x4>
#include <QVector3D>

//轨道相机 围绕目标点旋转/平移/缩放 鼠标左键旋转 右键平移 滚轮缩放
class GL3D_EXPORT Camera {
public:
    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projMatrix(float aspect) const;

    void orbit(float dx, float dy);      //弧度增量 yaw/pitch
    void pan(float dx, float dy);        //屏幕像素增量 换算到世界
    void zoom(float steps);              //滚轮步数 指数缩放保证远近手感一致
    void panLocal(float right, float up, float forward); //键盘WASD平移
    void fitToSphere(const QVector3D& center, float radius); //加载模型后自动取景

    QVector3D target() const { return m_target; }
    float distance() const { return m_distance; }

private:
    QVector3D position() const; //由球坐标还原眼位

    QVector3D m_target{0, 0, 0};
    float m_yaw = 0.6f;          //弧度 初始斜视展示立体感
    float m_pitch = 0.35f;
    float m_distance = 5.0f;
    float m_radius = 1.0f;       //模型半径 平移速度基准
};

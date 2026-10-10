#pragma once

#include "gl3d_export.h"
#include <QMatrix4x4>
#include <QVector3D>

//轨道相机 围绕目标点旋转/平移/缩放 鼠标左键旋转 右键平移 滚轮缩放
enum class UpAxis { Y, Z };

class GL3D_EXPORT Camera {
public:
    void setUpAxis(UpAxis axis);
    UpAxis upAxis() const { return m_upAxis; }
    QVector3D upVector() const { return m_upAxis == UpAxis::Z ? QVector3D(0,0,1) : QVector3D(0,1,0); }
    QVector3D groundPoint(float x, float second, float height = 0) const {
        return m_upAxis == UpAxis::Z ? QVector3D(x,second,height) : QVector3D(x,height,second);
    }
    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projMatrix(float aspect) const;

    void orbit(float dx, float dy);      //弧度增量 yaw/pitch
    void pan(float dx, float dy);        //屏幕像素增量 换算到世界
    void zoom(float steps);              //滚轮步数 指数缩放保证远近手感一致
    void panLocal(float right, float up, float forward); //键盘WASD平移
    void fitToSphere(const QVector3D& center, float radius); //加载模型后自动取景

    void setOrtho(bool on) { m_ortho = on; } //切换正交投影
    bool ortho() const { return m_ortho; }
    float fov() const { return m_fov; }

    QVector3D target() const { return m_target; }
    float distance() const { return m_distance; }
    QVector3D eye() const { return position(); } //眼位 操作器拾取射线用

private:
    QVector3D position() const; //由球坐标还原眼位

    UpAxis m_upAxis = UpAxis::Y;
    QVector3D m_target{0, 0, 0};
    float m_yaw = 0.6f;          //弧度 初始斜视展示立体感
    float m_pitch = 0.35f;
    float m_distance = 5.0f; //视距
    float m_radius = 1.0f;       //模型半径 平移速度基准
    bool m_ortho = false;        //false透视 true正交
    float m_fov = 45.0f;
};

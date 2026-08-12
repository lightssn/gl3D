#include "camera.h"
#include <cmath>
#include <algorithm>

QVector3D Camera::position() const {
    //球坐标→直角坐标 y轴为上
    float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    return m_target + QVector3D(m_distance * cp * sy, m_distance * sp, m_distance * cp * cy);
}

QMatrix4x4 Camera::viewMatrix() const {
    QMatrix4x4 v;
    v.lookAt(position(), m_target, QVector3D(0, 1, 0));
    return v;
}

QMatrix4x4 Camera::projMatrix(float aspect) const {
    QMatrix4x4 p;
    //近平面随距离自适应 避免大模型近裁剪 小模型深度精度差
    float nearP = std::max(m_distance * 0.01f, 0.001f);
    float farP = m_distance * 100.0f + 100.0f;
    if (m_ortho) {
        //正交视锥高随距离 与透视保持相近取景 滚轮缩放继续生效
        float halfH = m_distance * 0.5f;
        float halfW = halfH * aspect;
        p.ortho(-halfW, halfW, -halfH, halfH, nearP, farP);
    } else {
        p.perspective(45.0f, aspect, nearP, farP);
    }
    return p;
}

void Camera::orbit(float dx, float dy) {
    m_yaw += dx;
    //pitch钳制在±89° 防止万向锁翻转
    m_pitch = std::clamp(m_pitch + dy, -1.55f, 1.55f);
}

void Camera::pan(float dx, float dy) {
    //平移速度与距离成正比 远看移动快 近看精细
    float s = m_distance * 0.0018f;
    QVector3D fwd = (m_target - position()).normalized();
    QVector3D right = QVector3D::crossProduct(fwd, QVector3D(0, 1, 0)).normalized();
    QVector3D up = QVector3D::crossProduct(right, fwd);
    m_target += right * (-dx * s) + up * (dy * s);
}

void Camera::zoom(float steps) {
    m_distance *= std::pow(0.88f, steps);
    m_distance = std::clamp(m_distance, m_radius * 0.05f, m_radius * 100.0f + 10.0f);
}

void Camera::panLocal(float right, float up, float forward) {
    float s = m_distance * 0.03f;
    QVector3D fwd = (m_target - position()).normalized();
    QVector3D r = QVector3D::crossProduct(fwd, QVector3D(0, 1, 0)).normalized();
    QVector3D u = QVector3D::crossProduct(r, fwd);
    m_target += r * (right * s) + u * (up * s) + fwd * (forward * s);
}

void Camera::fitToSphere(const QVector3D& center, float radius) {
    m_target = center;
    m_radius = std::max(radius, 1e-4f);
    m_distance = radius * 2.5f; //45°视场下完整容纳模型
}

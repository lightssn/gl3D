#pragma once

#include "camera.h"

// Interaction-facing camera owner. Rendering backends only consume Camera's
// matrices; input policy stays outside the backend.
class GL3D_EXPORT CameraController {
public:
    void resetView(const QVector3D& center,float radius) { m_camera.fitToSphere(center,radius); }
    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }

private:
    Camera m_camera;
};

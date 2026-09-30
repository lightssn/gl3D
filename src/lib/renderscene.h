#pragma once

#include "gl3d_export.h"
#include "mesh.h"
#include "cameracontroller.h"
#include "selectioncontroller.h"
#include "renderoptions.h"
#include "osgbstream.h"
#include <memory>

// OpenGL 和 Vulkan 共享的场景状态。
// 这里不持有任何 API 对象，后端只负责把场景上传到自己的 GPU 资源。
class GL3D_EXPORT RenderScene final {
public:
    bool loadModel(const QString& path, QString* error = nullptr);
    void setMesh(Mesh mesh, const QString& path = QString());
    bool openOsgb(const QString& path, QString* error = nullptr);
    bool updateOsgb(const QSize& viewport);
    bool hasOsgb() const { return bool(m_osgb); }
    const OsgbStream* osgb() const { return m_osgb.get(); }
    void clear();
    void resetView();

    bool hasModel() const { return !m_mesh.subMeshes.empty() || hasOsgb(); }
    const QString& sourcePath() const { return m_sourcePath; }
    Mesh& mesh() { return m_mesh; }
    const Mesh& mesh() const { return m_mesh; }
    CameraController& cameraController() { return m_cameraController; }
    const CameraController& cameraController() const { return m_cameraController; }
    SelectionController& selection() { return m_selection; }
    const SelectionController& selection() const { return m_selection; }
    RenderOptions& options() { return m_options; }
    const RenderOptions& options() const { return m_options; }

private:
    Mesh m_mesh;
    CameraController m_cameraController;
    SelectionController m_selection;
    RenderOptions m_options;
    QString m_sourcePath;
    std::unique_ptr<OsgbStream> m_osgb;
    void replaceOsgbMesh(Mesh mesh);
};


#pragma once

#if defined(GL3D_HAS_VULKAN)

#include "gl3d_export.h"
#include "renderscene.h"
#include "renderview.h"
#include "debugstats.h"
#include <QVulkanWindow>
#include <QElapsedTimer>
#include <QQuaternion>
#include <QTimer>
#include <cstdint>
#include <vector>

class QVulkanInstance;

// Qt 管理 Vulkan surface、swapchain 和同步对象，渲染器负责录制绘制命令。
class GL3D_EXPORT VulkanWindow final : public QVulkanWindow, public RenderView {
    Q_OBJECT

public:
    explicit VulkanWindow(QVulkanInstance* instance, QWindow* parent = nullptr);
    ~VulkanWindow() override;

    bool isReady() const { return m_ready; }
    bool loadModel(const QString& path, QString* error = nullptr);
    void clearModel();
    void resetView();
    void setOrtho(bool enabled);
    void setWireframe(bool enabled);
    void setAntialiasing(bool enabled);
    bool supportsAntialiasing() const override { return m_multisamplingAvailable; }
    void setMipmaps(bool enabled);
    void setDepthTest(bool enabled);
    void setFaceCulling(bool enabled);
    void setPbr(bool enabled);
    void setNormalMap(bool enabled);
    void setFixedFpsEnabled(bool enabled);
    void setTargetFps(int fps);
    void setDebugView(int mode);
    void setOutlineWidth(float width);
    void setTransformMode(int mode);
    int transformMode() const { return m_transformMode; }
    void undo();
    void redo();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    int selectedSubMesh() const { return m_scene.selection().index(); }
    void selectSubMesh(int index);
    bool takePendingPick(QPoint& position);
    int pickSubMesh(const QPoint& pos) const;
    int subMeshCount() const { return int(m_scene.mesh().subMeshes.size()); }
    QString subMeshName(int index) const;
    bool subMeshVisible(int index) const;
    void setSubMeshVisible(int index, bool visible);
    float outlineWidth() const { return m_scene.options().outlineWidth; }
    DebugSnapshot debugSnapshot() override;
    void setClearColor(float red, float green, float blue) override;
    const float* clearColor() const { return m_clearColor; }
    QMatrix4x4 subMeshTransform(int index) const;
    QQuaternion subMeshRotation(int index) const;
    float gizmoRadius() const;
    void recordFrame(int drawCalls);
    void setResourceStats(qint64 vertexBytes, qint64 indexBytes, qint64 textureBytes,
                          qint64 textureCpuBytes, int textureCount, int bufferCount);
    void setFramebufferStats(qint64 bytes, int ownedCount);
    void setDeviceDescription(const QString& name, const QString& version);
    uint64_t modelRevision() const { return m_modelRevision; }
    uint64_t textureRevision() const { return m_textureRevision; }
    uint64_t pipelineRevision() const { return m_pipelineRevision; }
    bool mipmapsEnabled() const { return m_mipmaps; }
    bool multisamplingAvailable() const { return m_multisamplingAvailable; }
    const RenderScene& scene() const { return m_scene; }
    RenderScene& scene() { return m_scene; }
    RenderBackend backend() const override { return RenderBackend::Vulkan; }

signals:
    void fpsUpdated(int fps);
    void selectionChanged(bool selected);
    void subMeshSelected(int index);
    void historyChanged(bool canUndo, bool canRedo);
    void contextMenuRequested(const QPoint& globalPos);

protected:
    QVulkanWindowRenderer* createRenderer() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    bool m_ready = false;
    RenderScene m_scene;
    uint64_t m_modelRevision = 0;
    uint64_t m_textureRevision = 0;
    uint64_t m_pipelineRevision = 0;
    bool m_mipmaps = false;
    bool m_multisamplingAvailable = false;
    struct Bounds { QVector3D min, max; };
    struct TransformState { QVector3D position; QQuaternion rotation; QVector3D scale{1, 1, 1}; };
    std::vector<Bounds> m_bounds;
    std::vector<bool> m_visible;
    std::vector<TransformState> m_transforms;
    std::vector<TransformState> m_undo;
    std::vector<TransformState> m_redo;
    QPoint m_lastMousePos;
    QPoint m_pendingPick{-1, -1};
    Qt::MouseButton m_dragButton = Qt::NoButton;
    int m_dragDistance = 0;
    int m_dragAxis = 0;
    float m_dragAngle = 0.0f;
    int m_transformMode = 0;
    TransformState m_dragStart;
    QTimer m_renderTimer;
    QElapsedTimer m_fpsTimer;
    QElapsedTimer m_frameTimer;
    std::vector<float> m_frameTimes;
    int m_frameCount = 0;
    int m_lastFps = 0;
    int m_lastDrawCalls = 0;
    qint64 m_vertexBytes = 0, m_indexBytes = 0, m_textureBytes = 0, m_textureCpuBytes = 0;
    qint64 m_framebufferBytes = 0;
    int m_textureCount = 0, m_bufferCount = 0, m_ownedFramebufferCount = 0;
    QString m_deviceName, m_apiVersion;
    float m_clearColor[3] = {0.13f, 0.13f, 0.16f};
    int hitGizmo(const QPoint& pos) const;
    QVector3D gizmoAxis(int index) const;
};

#endif

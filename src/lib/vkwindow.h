#pragma once
#if defined(GL3D_HAS_VULKAN)
#include "gl3d_export.h"
#include "scene.h"
#include "editcontroller.h"
#include "debugstats.h"
#include <QVulkanWindow>
#include <QElapsedTimer>
#include <QQuaternion>
#include <QTimer>
#include <cstdint>
#include <vector>

class MeshLoader;

// Qt 管理 Vulkan surface、swapchain 和同步对象，渲染器负责录制绘制命令。
class GL3D_EXPORT VKWindow final : public QVulkanWindow, public Scene {
        Q_OBJECT

    public:
        explicit VKWindow(QVulkanInstance* instance, QWindow* parent = nullptr);
        ~VKWindow() override;

        bool isReady() const {
            return m_ready;
            }
        bool loadMeshAsync(const QStringList& paths);
        bool isLoading() const {
            return m_loading;
            }
        void meshUploadFinished();
        void updateTiles();
        void clearMesh();

        void requestRedraw() override {
            Scene::invalidateTiles();
            requestUpdate();
            }

        bool supportsAntialiasing() const override {
            return m_multisamplingAvailable;
            }

        bool takePendingPick(QPoint& position);
        int pickSubMesh(const QPoint& pos) const;

        DebugSnapshot debugSnapshot() override;

        float gizmoRadius() const;
        void recordFrame(int drawCalls);
        void setResourceStats(qint64 vertexBytes, qint64 indexBytes, qint64 textureBytes,
                              qint64 textureCpuBytes, int textureCount, int bufferCount);
        void setFramebufferStats(qint64 bytes, int ownedCount);
        void setDeviceDescription(const QString& name, const QString& version);
        uint64_t meshRevision() const {
            return m_modelRevision;
            }
        uint64_t textureRevision() const {
            return m_textureRevision;
            }
        uint64_t pipelineRevision() const {
            return m_pipelineRevision;
            }
        uint64_t gridRevision() const {
            return m_gridRevision;
            }

        bool multisamplingAvailable() const {
            return m_multisamplingAvailable;
            }
        RenderBackend backend() const override {
            return RenderBackend::Vulkan;
            }

    signals:
        void meshLoaded(const QString& info);
        void loadFailed(const QString& error);
        void loadStarted();
        void progressChanged(int percent);
        void loadFinished();
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
        bool m_loading = false;
        QThread* m_loadThread = nullptr;
        MeshLoader* m_loadWorker = nullptr;
        QString m_currentPath;
        uint64_t m_modelRevision = 0;
        uint64_t m_textureRevision = 0;
        uint64_t m_pipelineRevision = 0;
        uint64_t m_gridRevision = 0;
        bool m_multisamplingAvailable = false;
        using TransformState = EditTransform;

        QPoint m_lastMousePos;
        QPoint m_pendingPick{-1, -1};
        Qt::MouseButton m_dragButton = Qt::NoButton;
        int m_dragDistance = 0;
        int m_dragAxis = 0;
        float m_dragAngle = 0.0f;
        QTimer m_renderTimer;
        QTimer m_idleFpsTimer;
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
        void transformModeChanged() override { m_dragAxis=0; }
        void mipmapsChanged(bool) override { ++m_textureRevision; }
        QTimer* redrawTimer() override { return &m_renderTimer; }
        void pipelineOptionsChanged() override { ++m_pipelineRevision; }
        void overlaysChanged() override { ++m_gridRevision; }
        void notifyHistoryChanged(bool undo, bool redo) override { emit historyChanged(undo,redo); }
        void notifySelectionChanged(int index, bool changed) override {
            if(changed) emit selectionChanged(index>=0);
            emit subMeshSelected(index);
        }
        void onLoadFinished(bool ok, const QString& error, std::shared_ptr<Mesh> mesh);
        int hitGizmo(const QPoint& pos) const;
        QVector3D gizmoAxis(int index) const;
    };
#endif

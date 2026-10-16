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
        QThread* m_loadThread = nullptr;
        MeshLoader* m_loadWorker = nullptr;
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
        QTimer* redrawTimer() override { return &m_renderTimer; }
        void notifyLoadStarted() override { emit loadStarted(); }
        void notifyLoadProgress(int percent) override { emit progressChanged(percent); }
        void notifyMeshLoaded(const QString& info) override { emit meshLoaded(info); }
        void notifyLoadFailed(const QString& error) override { emit loadFailed(error); }
        void notifyLoadFinished() override { emit loadFinished(); }
        void resetInputState() override {
            m_dragAxis=0; m_dragButton=Qt::NoButton; m_pendingPick=QPoint(-1,-1);
        }
        void notifyHistoryChanged(bool undo, bool redo) override { emit historyChanged(undo,redo); }
        void notifySelectionChanged(int index, bool changed) override {
            if(changed) emit selectionChanged(index>=0);
            emit subMeshSelected(index);
        }
        int hitGizmo(const QPoint& pos) const;
        QVector3D gizmoAxis(int index) const;
    };
#endif

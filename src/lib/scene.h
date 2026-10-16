#pragma once

#include "gl3d_export.h"
#include "mesh.h"
#include "cameracontroller.h"
#include "selectioncontroller.h"
#include "renderoptions.h"
#include "tilestream.h"
#include "editcontroller.h"
#include "debugstats.h"

class QTimer;
enum class RenderBackend { OpenGL, Vulkan };

// 双后端共用的场景状态与操作流程，窗口实现通知和重绘钩子。
class GL3D_EXPORT Scene {
public:
    virtual ~Scene();
    Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    void setMesh(Mesh mesh, const QString& path = QString());
    void clear();
    void resetView();

    EditController& editor() { return m_editor; }
    const EditController& editor() const { return m_editor; }
    bool validObject(int index) const;
    bool selectObject(int index);
    EditTransform& transformState(int index) { return m_editor.state(index); }
    const EditTransform& transformState(int index) const { return m_editor.state(index); }
    QMatrix4x4 objectTransform(int index) const;
    void applyTransform(int index);
    bool objectVisible(int index) const;
    void setObjectVisible(int index, bool visible);
    void undo();
    void redo();

    bool hasMesh() const { return m_mesh.tileRoot || !m_mesh.subMeshes.empty(); }
    bool tiled() const { return bool(m_stream); }
    int tileRepresentative(int slot) const;
    bool sameTile(int a, int b) const;
    void updateTiles(const QSize& viewport);
    bool needsTileRedraw() const;
    void invalidateTiles() { m_tileFramePending=true; }
    const std::vector<QVector3D>& tileBoundsLines() const;
    uint64_t tileBoundsRevision() const { return m_stream ? m_stream->boundsRevision() : 0; }
    void markUploaded(int slot);
    bool drawable(int slot) const;
    static QVector3D tileLevelColor(int level);
    QVector4D tileTint(int slot) const;
    bool tileVisible(int slot) const;
    void setTileVisible(int slot, bool visible);
    QMatrix4x4 tileTransform(int slot) const;
    void setTileTransform(int slot, const QMatrix4x4& matrix);
    TileStats tileStats() const { return m_stream ? m_stream->stats() : TileStats(); }
    bool tileBudgetEviction() const { return m_stream && m_stream->budgetEviction; }
    qint64 tileBudgetBytes() const { return m_stream ? m_stream->budgetBytes : 1024ll*1024*1024; }
    void setTileCachePolicy(qint64 budget, bool eviction) { if(m_stream) { m_stream->budgetBytes=std::max(qint64(0),budget); m_stream->budgetEviction=eviction; invalidateTiles(); } }
    std::vector<TileLevelSettings> tileLevels() const { return m_stream ? m_stream->levels() : std::vector<TileLevelSettings>(); }
    void setTileLevel(int level, bool enabled, float minimum, float maximum, bool overrideRange) { if(m_stream) { m_stream->setLevel(level,enabled,minimum,maximum,overrideRange); invalidateTiles(); } }
    void resetTileLevels() { if(m_stream) { m_stream->resetLevels(); invalidateTiles(); } }
    const QString& sourcePath() const { return m_sourcePath; }
    Mesh& mesh() { return m_mesh; }
    const Mesh& mesh() const { return m_mesh; }
    CameraController& cameraController() { return m_cameraController; }
    const CameraController& cameraController() const { return m_cameraController; }
    SelectionController& selection() { return m_selection; }
    const SelectionController& selection() const { return m_selection; }
    RenderOptions& options() { return m_options; }
    const RenderOptions& options() const { return m_options; }

    virtual RenderBackend backend() const { return RenderBackend::OpenGL; }
    virtual DebugSnapshot debugSnapshot() { return {}; }
    virtual void requestRedraw() { invalidateTiles(); }
    void setUpAxis(UpAxis axis);
    void setOrtho(bool enabled);
    void setWireframe(bool enabled);
    void setTileBounds(bool enabled);
    void setAntialiasing(bool enabled);
    virtual bool supportsAntialiasing() const { return true; }
    virtual bool supportsPbr() const { return !tiled(); }
    virtual bool supportsNormalMap() const { return !tiled(); }
    void setMipmaps(bool enabled);
    void setDepthTest(bool enabled);
    void setFaceCulling(bool enabled);
    void setPbr(bool enabled);
    void setNormalMap(bool enabled);
    void setNormalLighting(bool enabled) { options().normalLighting = enabled; requestRedraw(); }
    void setFixedFpsEnabled(bool enabled);
    void setTargetFps(int fps);
    void setDebugView(int mode);
    float outlineWidth() const;
    void setOutlineWidth(float width);
    int subMeshCount() const;
    QString subMeshName(int index) const;
    bool subMeshVisible(int index) const;
    void setSubMeshVisible(int index, bool visible);
    int selectedSubMesh() const;
    void selectSubMesh(int index);
    void setTransformMode(int mode);
    void setClearColor(float red, float green, float blue);
    int transformMode() const { return m_editor.mode; }
    const float* clearColor() const { return m_options.clearColor; }
    bool mipmapsEnabled() const { return options().mipmaps; }
    bool canUndo() const { return editor().canUndo(); }
    bool canRedo() const { return editor().canRedo(); }
    QMatrix4x4 subMeshTransform(int index) const { return objectTransform(index); }
    QQuaternion subMeshRotation(int index) const { return transformState(index).rotation; }
    void setShowGrid(bool enabled);
    void setShowGizmo(bool enabled);
    void setShowSelection(bool enabled);
    void setShowAxes(bool enabled);
    void beginEdit();
    void finishEdit(EditController::ChangePolicy policy);

protected:
    virtual void transformModeChanged() {}
    virtual void antialiasingChanged() {}
    virtual void mipmapsChanged(bool) {}
    virtual QTimer* redrawTimer() { return nullptr; }
    virtual void transformsChanged() {}
    virtual void visibilityChanged(int, bool) {}
    virtual void pipelineOptionsChanged() {}
    virtual void overlaysChanged() {}
    virtual void notifyHistoryChanged(bool, bool) {}
    virtual void notifySelectionChanged(int, bool) {}

private:
    bool m_tileFramePending = true;
    QMatrix4x4 m_tileView, m_tileProjection;
    void syncEditObjects();
    EditController m_editor;
    std::vector<bool> m_visibility;
    Mesh m_mesh;
    CameraController m_cameraController;
    SelectionController m_selection;
    RenderOptions m_options;
    QString m_sourcePath;
    // Stop paging before destroying the state it uses.
    std::unique_ptr<TileStream> m_stream;
};

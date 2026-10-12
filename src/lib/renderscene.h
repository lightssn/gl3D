#pragma once

#include "gl3d_export.h"
#include "mesh.h"
#include "cameracontroller.h"
#include "selectioncontroller.h"
#include "renderoptions.h"
#include "tilestream.h"

// OpenGL 和 Vulkan 共享的场景状态。
// 这里不持有任何 API 对象，后端只负责把场景上传到自己的 GPU 资源。
class GL3D_EXPORT RenderScene final {
public:
    ~RenderScene();
    bool loadModel(const QString& path, QString* error = nullptr);
    void setMesh(Mesh mesh, const QString& path = QString());
    void clear();
    void resetView();

    bool hasModel() const { return m_mesh.tileRoot || !m_mesh.subMeshes.empty(); }
    bool tiled() const { return bool(m_stream); }
    int tileRepresentative(int slot) const;
    bool sameTile(int a, int b) const;
    void updateTiles(const QSize& viewport);
    bool needsTileRedraw() const;
    void invalidateTiles() { m_tileFramePending=true; }
    const std::vector<QVector3D>& tileBoundsLines() const { return m_tileBoundsLines; }
    uint64_t tileBoundsRevision() const { return m_tileBoundsRevision; }
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

private:
    bool m_tileFramePending = true;
    QMatrix4x4 m_tileView, m_tileProjection;
    void refreshTileBounds();
    std::vector<QVector3D> m_tileBoundsLines;
    uint64_t m_tileBoundsRevision = 0;
    void refreshTileRepresentatives();
    std::map<int, int> m_tileRepresentatives;
    std::unique_ptr<TileStream> m_stream;
    Mesh m_mesh;
    CameraController m_cameraController;
    SelectionController m_selection;
    RenderOptions m_options;
    QString m_sourcePath;
};

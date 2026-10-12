#include "renderscene.h"
#include "meshloader.h"
#include <algorithm>
#include <cfloat>
#include <utility>

RenderScene::~RenderScene() = default;
int RenderScene::tileRepresentative(int slot) const {
    if(!tiled() || slot<0 || slot>=int(m_mesh.subMeshes.size())) return slot;
    const int owner=m_mesh.subMeshes[size_t(slot)].tileOwner;
    auto found = m_tileRepresentatives.find(owner);
    return found == m_tileRepresentatives.end() ? slot : found->second;
}
void RenderScene::refreshTileRepresentatives() {
    m_tileRepresentatives.clear();
    if (!tiled()) return;
    for (int i=0; i<int(m_mesh.subMeshes.size()); ++i) {
        const auto& part = m_mesh.subMeshes[size_t(i)];
        if (part.resident) m_tileRepresentatives.emplace(part.tileOwner, i);
    }
}
bool RenderScene::sameTile(int a,int b) const {
    if(a==b) return true;
    return tiled() && a>=0 && b>=0 && a<int(m_mesh.subMeshes.size()) && b<int(m_mesh.subMeshes.size()) &&
        m_mesh.subMeshes[size_t(a)].tileOwner==m_mesh.subMeshes[size_t(b)].tileOwner;
}
void RenderScene::updateTiles(const QSize& viewport) {
    if (!m_stream) return;
    m_tileFramePending=false;
    m_tileView=m_cameraController.camera().viewMatrix();
    m_tileProjection=m_cameraController.camera().projMatrix(1);
    m_stream->update(m_mesh, m_cameraController.camera(), viewport, m_selection.index());
    refreshTileRepresentatives();
    refreshTileBounds();
    auto& centers=m_selection.centers(); centers.resize(m_mesh.subMeshes.size());
    for(size_t i=0;i<centers.size();++i) centers[i]=m_stream->center(m_mesh.subMeshes[i].tileOwner);
}
bool RenderScene::needsTileRedraw() const {
    if(!m_stream) return false;
    return !m_options.osgbOnDemand || m_tileFramePending || m_stream->needsUpdate() ||
        m_tileView!=m_cameraController.camera().viewMatrix() || m_tileProjection!=m_cameraController.camera().projMatrix(1);
}
void RenderScene::refreshTileBounds() {
    std::vector<QVector3D> lines;
    if(m_stream && m_options.showTileBounds) {
        std::function<void(const std::shared_ptr<TileNode>&)> collect=[&](const auto& node) {
            QVector3D a(FLT_MAX,FLT_MAX,FLT_MAX), b(-FLT_MAX,-FLT_MAX,-FLT_MAX);
            bool found=false;
            for(int slot:node->renderSlots) {
                const auto& part=m_mesh.subMeshes[size_t(slot)];
                if(!part.resident || !part.drawable) continue;
                found=true;
                for(int k=0;k<3;++k) { a[k]=std::min(a[k],part.boundsMin[k]); b[k]=std::max(b[k],part.boundsMax[k]); }
            }
            if(found) {
                QVector3D p[8]={{a.x(),a.y(),a.z()},{b.x(),a.y(),a.z()},
                    {b.x(),b.y(),a.z()},{a.x(),b.y(),a.z()},{a.x(),a.y(),b.z()},
                    {b.x(),a.y(),b.z()},{b.x(),b.y(),b.z()},{a.x(),b.y(),b.z()}};
                const auto matrix=m_stream->transform(node->owner);
                for(auto& point:p) point=matrix.map(point);
                for(int e=0;e<12;++e) {
                    const int u=e<8?e:e-8;
                    const int v=e<4?(e+1)%4:e<8?(e-3)%4+4:e-8+4;
                    lines.push_back(p[u]); lines.push_back(p[v]);
                }
            }
            for(const auto& child:node->children) collect(child);
        }; collect(m_mesh.tileRoot);
    }
    if(lines!=m_tileBoundsLines) { m_tileBoundsLines=std::move(lines); ++m_tileBoundsRevision; }
}
void RenderScene::markUploaded(int slot) {
    if(m_stream) { m_stream->uploaded(m_mesh,slot); invalidateTiles(); }
}
bool RenderScene::drawable(int slot) const {
    return slot>=0 && slot<int(m_mesh.subMeshes.size()) && (!m_stream || m_mesh.subMeshes[size_t(slot)].drawable);
}
bool RenderScene::tileVisible(int slot) const {
    return slot>=0 && slot<int(m_mesh.subMeshes.size()) && m_mesh.subMeshes[size_t(slot)].resident &&
        (!m_stream || m_stream->visible(m_mesh.subMeshes[size_t(slot)].tileOwner));
}
void RenderScene::setTileVisible(int slot,bool visible) { if(m_stream && slot>=0 && slot<int(m_mesh.subMeshes.size())) { m_stream->setVisible(m_mesh.subMeshes[size_t(slot)].tileOwner,visible); invalidateTiles(); } }
QMatrix4x4 RenderScene::tileTransform(int slot) const {
    return m_stream && slot>=0 && slot<int(m_mesh.subMeshes.size())?m_stream->transform(m_mesh.subMeshes[size_t(slot)].tileOwner):QMatrix4x4();
}
void RenderScene::setTileTransform(int slot,const QMatrix4x4& matrix) {
    if(m_stream && slot>=0 && slot<int(m_mesh.subMeshes.size())) { m_stream->setTransform(m_mesh.subMeshes[size_t(slot)].tileOwner,matrix); invalidateTiles(); }
}

bool RenderScene::loadModel(const QString& path, QString* error)
{
    Mesh mesh;
    if (!MeshLoader::load(path, mesh, error))
        return false;
    setMesh(std::move(mesh), path);
    return true;
}

void RenderScene::setMesh(Mesh mesh, const QString& path)
{
    m_stream.reset();
    invalidateTiles();
    // Value copies share the source tree; paging state belongs to this scene.
    if (mesh.tileRoot && mesh.tileRoot.use_count() > 1) {
        std::function<std::shared_ptr<TileNode>(const std::shared_ptr<TileNode>&)> clone = [&](const auto& node) {
            auto copy = std::make_shared<TileNode>(*node);
            copy->children.clear();
            for (const auto& child : node->children) copy->children.push_back(clone(child));
            return copy;
        };
        mesh.tileRoot = clone(mesh.tileRoot);
    }
    m_mesh = std::move(mesh);
    m_cameraController.camera().setUpAxis(m_mesh.tileRoot ? UpAxis::Z : UpAxis::Y);
    if(m_mesh.tileRoot) m_stream=std::make_unique<TileStream>(m_mesh);
    refreshTileRepresentatives();
    refreshTileBounds();
    m_sourcePath = path;
    m_selection.indexRef() = -1;
    m_selection.centers().clear();
    m_selection.centers().reserve(m_mesh.subMeshes.size());
    for (const auto& subMesh : m_mesh.subMeshes) {
        if(m_stream) {
            m_selection.centers().push_back(m_stream->center(subMesh.tileOwner));
            continue;
        }
        if (subMesh.vertices.empty()) {
            m_selection.centers().push_back(m_mesh.center());
            continue;
        }
        QVector3D minP(FLT_MAX, FLT_MAX, FLT_MAX);
        QVector3D maxP(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const Vertex& vertex : subMesh.vertices) {
            const QVector3D p(vertex.px, vertex.py, vertex.pz);
            minP.setX(std::min(minP.x(), p.x()));
            minP.setY(std::min(minP.y(), p.y()));
            minP.setZ(std::min(minP.z(), p.z()));
            maxP.setX(std::max(maxP.x(), p.x()));
            maxP.setY(std::max(maxP.y(), p.y()));
            maxP.setZ(std::max(maxP.z(), p.z()));
        }
        m_selection.centers().push_back((minP + maxP) * 0.5f);
    }
    resetView();
}

void RenderScene::clear()
{
    m_stream.reset();
    m_mesh = Mesh();
    refreshTileBounds();
    m_tileRepresentatives.clear();
    m_sourcePath.clear();
    m_selection.indexRef() = -1;
    m_selection.centers().clear();
}

void RenderScene::resetView()
{
    if (hasModel())
        m_cameraController.camera().fitToSphere(m_mesh.center(), m_mesh.radius());
}

QVector3D RenderScene::tileLevelColor(int level) {
    // Fixed L0..L5 palette remains stable as deeper pages are discovered.
    const float t=std::clamp(level/5.f,0.f,1.f);
    const QVector3D green(0.65f,1.f,0.65f), yellow(1.f,1.f,0.65f), red(1.f,0.65f,0.65f);
    return t<=0.5f ? green*(1-2*t)+yellow*(2*t) : yellow*(2-2*t)+red*(2*t-1);
}
QVector4D RenderScene::tileTint(int slot) const {
    if(!m_stream || !m_options.tileLevelTint || slot<0 || slot>=int(m_mesh.subMeshes.size())) return {};
    const auto& part=m_mesh.subMeshes[size_t(slot)];
    if(!part.resident || part.tileLevel<0) return {};
    return QVector4D(tileLevelColor(part.tileLevel),0.45f);
}

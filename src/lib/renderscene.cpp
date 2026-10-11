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
    m_stream->update(m_mesh, m_cameraController.camera(), viewport, m_selection.index());
    refreshTileRepresentatives();
    auto& centers=m_selection.centers(); centers.resize(m_mesh.subMeshes.size());
    for(size_t i=0;i<centers.size();++i) centers[i]=m_stream->center(m_mesh.subMeshes[i].tileOwner);
}
void RenderScene::markUploaded(int slot) { if(m_stream) m_stream->uploaded(m_mesh,slot); }
bool RenderScene::drawable(int slot) const {
    return slot>=0 && slot<int(m_mesh.subMeshes.size()) && (!m_stream || m_mesh.subMeshes[size_t(slot)].drawable);
}
bool RenderScene::tileVisible(int slot) const {
    return slot>=0 && slot<int(m_mesh.subMeshes.size()) && m_mesh.subMeshes[size_t(slot)].resident &&
        (!m_stream || m_stream->visible(m_mesh.subMeshes[size_t(slot)].tileOwner));
}
void RenderScene::setTileVisible(int slot,bool visible) { if(m_stream && slot>=0 && slot<int(m_mesh.subMeshes.size())) m_stream->setVisible(m_mesh.subMeshes[size_t(slot)].tileOwner,visible); }
QMatrix4x4 RenderScene::tileTransform(int slot) const {
    return m_stream && slot>=0 && slot<int(m_mesh.subMeshes.size())?m_stream->transform(m_mesh.subMeshes[size_t(slot)].tileOwner):QMatrix4x4();
}
void RenderScene::setTileTransform(int slot,const QMatrix4x4& matrix) {
    if(m_stream && slot>=0 && slot<int(m_mesh.subMeshes.size())) m_stream->setTransform(m_mesh.subMeshes[size_t(slot)].tileOwner,matrix);
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
<<<<<<< HEAD
    m_stream.reset();
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
=======
    m_osgb.reset();
>>>>>>> d8eb4ffe194d0afa7bf43d9f71952bfa2058c50e
    m_mesh = std::move(mesh);
    m_cameraController.camera().setUpAxis(m_mesh.tileRoot ? UpAxis::Z : UpAxis::Y);
    if(m_mesh.tileRoot) m_stream=std::make_unique<TileStream>(m_mesh);
    refreshTileRepresentatives();
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
<<<<<<< HEAD
    m_stream.reset();
=======
    m_osgb.reset();
>>>>>>> d8eb4ffe194d0afa7bf43d9f71952bfa2058c50e
    m_mesh = Mesh();
    m_tileRepresentatives.clear();
    m_sourcePath.clear();
    m_selection.indexRef() = -1;
    m_selection.centers().clear();
}

bool RenderScene::openOsgb(const QString& path, QString* error)
{
    auto stream = std::make_unique<OsgbStream>();
    if (!stream->open(path, error)) return false;
    m_osgb = std::move(stream);
    m_sourcePath = path;
    replaceOsgbMesh(m_osgb->visibleMesh());
    m_cameraController.camera().fitToSphere(m_osgb->rootCenter(), std::max(m_osgb->rootRadius(), 1.0f));
    return true;
}

void RenderScene::replaceOsgbMesh(Mesh mesh)
{
    m_mesh = std::move(mesh);
    m_selection.indexRef() = -1;
    m_selection.centers().clear();
    const auto& mins = m_osgb->tileMins();
    const auto& maxs = m_osgb->tileMaxs();
    for (size_t i = 0; i < mins.size(); ++i)
        m_selection.centers().push_back((mins[i] + maxs[i]) * 0.5f);
}

bool RenderScene::updateOsgb(const QSize& viewport)
{
    if (!m_osgb || !m_osgb->update(m_cameraController.camera(), viewport)) return false;
    replaceOsgbMesh(m_osgb->visibleMesh());
    return true;
}

void RenderScene::resetView()
{
    if (m_osgb) {
        m_cameraController.camera().fitToSphere(m_osgb->rootCenter(), std::max(m_osgb->rootRadius(), 1.0f));
        return;
    }
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

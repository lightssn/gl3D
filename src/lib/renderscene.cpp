#include "renderscene.h"
#include "meshloader.h"
#include <algorithm>
#include <cfloat>
#include <utility>

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
    m_osgb.reset();
    m_mesh = std::move(mesh);
    m_sourcePath = path;
    m_selection.indexRef() = -1;
    m_selection.centers().clear();
    m_selection.centers().reserve(m_mesh.subMeshes.size());
    for (const auto& subMesh : m_mesh.subMeshes) {
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
    m_osgb.reset();
    m_mesh = Mesh();
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

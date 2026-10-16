#include "scene.h"
#include <algorithm>
#include <cfloat>
#include <utility>
#include <QTimer>
#include <QFileInfo>

Scene::~Scene() = default;

bool Scene::beginLoad(const QStringList& paths) {
    // A discarded read still owns the worker until its result arrives.
    if(m_loadStage!=LoadStage::Idle || paths.isEmpty()) return false;
    m_loadPath=paths.size()==1?paths.front():QStringLiteral("OSGB (%1 files)").arg(paths.size());
    m_loadStage=LoadStage::Reading;
    notifyLoadStarted();
    notifyLoadProgress(0);
    return true;
}

void Scene::reportLoadProgress(int percent) {
    if(m_loadStage==LoadStage::Reading) notifyLoadProgress(std::clamp(percent,0,100)*9/10);
}

void Scene::acceptLoadResult(bool ok,const QString& error,std::shared_ptr<Mesh> mesh) {
    if(m_loadStage==LoadStage::Discarding) { m_loadStage=LoadStage::Idle; return; }
    if(m_loadStage!=LoadStage::Reading) return;
    if(!ok || !mesh) {
        m_loadStage=LoadStage::Idle;
        notifyLoadFailed(error.isEmpty()?QStringLiteral("加载失败"):error);
        notifyLoadFinished();
        requestRedraw();
        return;
    }
    const QString unavailable=meshAvailabilityError();
    if(!unavailable.isEmpty()) {
        m_loadStage=LoadStage::Idle;
        notifyLoadUnavailable(unavailable);
        notifyLoadFinished();
        return;
    }
    m_loadStage=LoadStage::Uploading;
    setMesh(std::move(*mesh),m_loadPath);
    if(m_loadStage!=LoadStage::Uploading) return;
    notifyMeshLoaded(QStringLiteral("%1  顶点%2  三角%3  子网格%4")
        .arg(QFileInfo(m_loadPath).fileName()).arg(m_mesh.vertexCount()).arg(m_mesh.triangleCount()).arg(subMeshCount()));
    notifyLoadProgress(90);
    requestRedraw();
}

void Scene::finishUpload() {
    if(m_loadStage!=LoadStage::Uploading) return;
    m_loadStage=LoadStage::Idle;
    notifyLoadProgress(100);
    notifyLoadFinished();
}

int Scene::tileRepresentative(int slot) const {
    return m_stream ? m_stream->representative(m_mesh,slot) : slot;
}
const std::vector<QVector3D>& Scene::tileBoundsLines() const {
    static const std::vector<QVector3D> empty;
    return m_stream ? m_stream->boundsLines() : empty;
}
bool Scene::sameTile(int a,int b) const {
    if(a==b) return true;
    return tiled() && a>=0 && b>=0 && a<int(m_mesh.subMeshes.size()) && b<int(m_mesh.subMeshes.size()) &&
           m_mesh.subMeshes[size_t(a)].tileOwner==m_mesh.subMeshes[size_t(b)].tileOwner;
    }
void Scene::updateTiles(const QSize& viewport) {
    if (!m_stream) return;
    m_tileFramePending=false;
    m_tileView=m_cameraController.camera().viewMatrix();
    m_tileProjection=m_cameraController.camera().projMatrix(1);
    m_stream->update(m_mesh, m_cameraController.camera(), viewport, m_selection.index());
    const auto boundsRevision=m_stream->boundsRevision();
    m_stream->refreshDisplay(m_mesh,m_options.showTileBounds);
    if(boundsRevision!=m_stream->boundsRevision()) m_changes.tileBounds=true;
    syncEditObjects();
    auto& centers=m_selection.centers();
    centers.resize(m_mesh.subMeshes.size());
    for(size_t i=0; i<centers.size(); ++i) centers[i]=m_stream->center(m_mesh.subMeshes[i].tileOwner);
    }
bool Scene::needsTileRedraw() const {
    if(!m_stream) return false;
    return !m_options.osgbOnDemand || m_tileFramePending || m_stream->needsUpdate() ||
    m_tileView!=m_cameraController.camera().viewMatrix() || m_tileProjection!=m_cameraController.camera().projMatrix(1);
    }
void Scene::markUploaded(int slot) {
    if(m_stream) {
        m_stream->uploaded(m_mesh,slot);
        invalidateTiles();
        }
    }
bool Scene::drawable(int slot) const {
    return slot>=0 && slot<int(m_mesh.subMeshes.size()) && (!m_stream || m_mesh.subMeshes[size_t(slot)].drawable);
    }
bool Scene::tileVisible(int slot) const {
    return slot>=0 && slot<int(m_mesh.subMeshes.size()) && m_mesh.subMeshes[size_t(slot)].resident &&
           (!m_stream || m_stream->visible(m_mesh.subMeshes[size_t(slot)].tileOwner));
    }
void Scene::setTileVisible(int slot,bool visible) {
    if(m_stream && slot>=0 && slot<int(m_mesh.subMeshes.size())) {
        m_stream->setVisible(m_mesh.subMeshes[size_t(slot)].tileOwner,visible);
        invalidateTiles();
        }
    }
QMatrix4x4 Scene::tileTransform(int slot) const {
    return m_stream && slot>=0 && slot<int(m_mesh.subMeshes.size())?m_stream->transform(m_mesh.subMeshes[size_t(slot)].tileOwner):QMatrix4x4();
    }
void Scene::setTileTransform(int slot,const QMatrix4x4& matrix) {
    if(m_stream && slot>=0 && slot<int(m_mesh.subMeshes.size())) {
        m_stream->setTransform(m_mesh.subMeshes[size_t(slot)].tileOwner,matrix);
        invalidateTiles();
        }
    }

void Scene::setMesh(Mesh mesh, const QString& path) {
    m_changes.mesh=m_changes.grid=m_changes.gizmo=m_changes.tileBounds=true;
    m_changes.objects.clear();
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
    m_editor.reset();
    m_mesh = std::move(mesh);
    if(m_mesh.tileRoot) { m_options.pbr=false; m_options.normalMap=false; }
    m_options.outlineWidth=RenderOptions{}.outlineWidth;
    m_visibility.assign(m_mesh.subMeshes.size(),true);
    m_cameraController.camera().setUpAxis(m_mesh.tileRoot ? UpAxis::Z : UpAxis::Y);
    if(m_mesh.tileRoot) m_stream=std::make_unique<TileStream>(m_mesh);
    if(m_stream) m_stream->refreshDisplay(m_mesh,m_options.showTileBounds);
    m_sourcePath = path;
    m_selection.indexRef() = -1;
    m_selection.centers().clear();
    m_selection.centers().reserve(m_mesh.subMeshes.size());
    for (auto& subMesh : m_mesh.subMeshes) {
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
        subMesh.boundsMin=minP; subMesh.boundsMax=maxP;
        m_selection.centers().push_back((minP + maxP) * 0.5f);
        }
    syncEditObjects();
    resetInputState();
    notifyHistoryChanged(false,false);
    notifySelectionChanged(-1,true);
    resetView();
    }

void Scene::clear() {
    const bool loading=isLoading();
    if(m_loadStage==LoadStage::Reading) m_loadStage=LoadStage::Discarding;
    else if(m_loadStage==LoadStage::Uploading) m_loadStage=LoadStage::Idle;
    m_changes.mesh=m_changes.grid=m_changes.gizmo=m_changes.tileBounds=true;
    m_changes.objects.clear();
    m_editor.reset();
    m_visibility.clear();
    m_stream.reset();
    m_mesh = Mesh();

    m_sourcePath.clear();
    m_selection.indexRef() = -1;
    m_selection.centers().clear();
    m_editor.mode=0;
    resetInputState();
    notifyHistoryChanged(false,false);
    notifySelectionChanged(-1,true);
    if(loading) notifyLoadFinished();
    requestRedraw();
    }

void Scene::resetView() {
    if (hasMesh())
        m_cameraController.resetView(m_mesh.center(), m_mesh.radius());
    requestRedraw();
    }

QVector3D Scene::tileLevelColor(int level) {
    // Fixed L0..L5 palette remains stable as deeper pages are discovered.
    const float t=std::clamp(level/5.f,0.f,1.f);
    const QVector3D green(0.65f,1.f,0.65f), yellow(1.f,1.f,0.65f), red(1.f,0.65f,0.65f);
    return t<=0.5f ? green*(1-2*t)+yellow*(2*t) : yellow*(2-2*t)+red*(2*t-1);
    }
QVector4D Scene::tileTint(int slot) const {
    if(!m_stream || !m_options.tileLevelTint || slot<0 || slot>=int(m_mesh.subMeshes.size())) return {};
    const auto& part=m_mesh.subMeshes[size_t(slot)];
    if(!part.resident || part.tileLevel<0) return {};
    return QVector4D(tileLevelColor(part.tileLevel),0.45f);
    }

void Scene::syncEditObjects() {
    m_editor.resizeObjects(m_mesh.subMeshes.size());
    for(size_t i=0;i<m_mesh.subMeshes.size();++i)
        m_editor.setOwner(int(i),tiled()?m_mesh.subMeshes[i].tileOwner:-1);
}

bool Scene::validObject(int index) const {
    return index>=0 && index<int(m_mesh.subMeshes.size()) && (!tiled() || m_mesh.subMeshes[size_t(index)].resident);
}

bool Scene::selectObject(int index) {
    if(index!=-1 && !validObject(index)) return false;
    if(m_selection.index()==index) return false;
    m_selection.indexRef()=index;
    m_editor.selectState(index);
    m_changes.gizmo=true;
    return true;
}

QMatrix4x4 Scene::objectTransform(int index) const {
    if(!validObject(index)) return {};
    if(tiled()) return tileTransform(index);
    const auto& centers=m_selection.centers();
    const QVector3D center=index<int(centers.size())?centers[size_t(index)]:m_mesh.center();
    return transformState(index).matrix(center);
}

void Scene::applyTransform(int index) {
    if(!validObject(index)) return;
    if(tiled()) setTileTransform(index,transformState(index).matrix(m_selection.centers()[size_t(index)]));
    m_changes.objects.insert(index); m_changes.gizmo=true;
}

bool Scene::objectVisible(int index) const {
    if(!validObject(index)) return false;
    return tiled()?tileVisible(index):(index<int(m_visibility.size()) && m_visibility[size_t(index)]);
}

void Scene::setObjectVisible(int index,bool visible) {
    if(!validObject(index)) return;
    if(tiled()) setTileVisible(index,visible);
    else m_visibility[size_t(index)]=visible;
    m_changes.objects.insert(index);
}


void Scene::setUpAxis(UpAxis axis) {
    auto& camera=cameraController().camera();
    if(camera.upAxis()==axis) return;
    camera.setUpAxis(axis); m_changes.grid=true; requestRedraw();
}
void Scene::setOrtho(bool enabled) { cameraController().camera().setOrtho(enabled); requestRedraw(); }
void Scene::setWireframe(bool enabled) { options().wireframe=enabled; m_changes.drawOptions=true; requestRedraw(); }
void Scene::setTileBounds(bool enabled) { options().showTileBounds=enabled; m_changes.grid=true; requestRedraw(); }
void Scene::setDepthTest(bool enabled) { options().depthTest=enabled; m_changes.drawOptions=true; requestRedraw(); }
void Scene::setFaceCulling(bool enabled) { options().faceCulling=enabled; m_changes.drawOptions=true; requestRedraw(); }
void Scene::setPbr(bool enabled) { options().pbr=enabled && supportsPbr(); requestRedraw(); }
void Scene::setNormalMap(bool enabled) { options().normalMap=enabled && supportsNormalMap(); requestRedraw(); }
void Scene::setDebugView(int mode) { options().debugView=std::clamp(mode,0,2); requestRedraw(); }
float Scene::outlineWidth() const { return options().outlineWidth; }
void Scene::setOutlineWidth(float width) { options().outlineWidth=std::max(0.0f,width); requestRedraw(); }
void Scene::setShowGrid(bool enabled) { options().showGrid=enabled; requestRedraw(); }
void Scene::setShowGizmo(bool enabled) { options().showGizmo=enabled; requestRedraw(); }
void Scene::setShowSelection(bool enabled) { options().showSelection=enabled; requestRedraw(); }
void Scene::setShowAxes(bool enabled) { options().showAxes=enabled; requestRedraw(); }

void Scene::setFixedFpsEnabled(bool enabled) {
    options().fixedFps=enabled;
    if(enabled) setTargetFps(options().targetFps);
    else if(auto* timer=redrawTimer()) timer->stop();
    requestRedraw();
}
void Scene::setTargetFps(int fps) {
    const int value=options().targetFps=std::max(0,fps);
    if(options().fixedFps) if(auto* timer=redrawTimer()) timer->start(value>0?std::max(1,1000/value):0);
}
int Scene::subMeshCount() const { return int(mesh().subMeshes.size()); }
QString Scene::subMeshName(int index) const {
    if(index<0 || index>=subMeshCount()) return {};
    const auto& name=mesh().subMeshes[size_t(index)].materialName;
    return name.empty()?QStringLiteral("(默认材质)"):QString::fromStdString(name);
}
bool Scene::subMeshVisible(int index) const { return objectVisible(index); }
void Scene::setSubMeshVisible(int index,bool visible) {
    if(!validObject(index)) return;
    setObjectVisible(index,visible); requestRedraw();
}
int Scene::selectedSubMesh() const { return selection().index(); }
void Scene::selectSubMesh(int index) {
    if(index!=-1 && !validObject(index)) return;
    const bool changed=selectObject(index);
    if(changed) notifyHistoryChanged(canUndo(),canRedo());
    notifySelectionChanged(index,changed);
    if(changed) requestRedraw();
}
void Scene::undo() {
    const int index=selectedSubMesh();
    if(!validObject(index) || !m_editor.undo(index,transformState(index))) return;
    applyTransform(index);
    notifyHistoryChanged(canUndo(),canRedo()); requestRedraw();
}
void Scene::redo() {
    const int index=selectedSubMesh();
    if(!validObject(index) || !m_editor.redo(index,transformState(index))) return;
    applyTransform(index);
    notifyHistoryChanged(canUndo(),canRedo()); requestRedraw();
}
void Scene::beginEdit() {
    const int index=selectedSubMesh();
    if(validObject(index)) editor().beginDrag(transformState(index));
}
void Scene::finishEdit(EditController::ChangePolicy policy) {
    const int index=selectedSubMesh();
    if(!validObject(index)) return;
    applyTransform(index);
    editor().endDrag(index,transformState(index),policy);
    notifyHistoryChanged(canUndo(),canRedo());
    requestRedraw();
}

void Scene::setTransformMode(int mode) {
    m_editor.mode=std::clamp(mode,0,3); m_changes.gizmo=true; transformModeChanged(); requestRedraw();
}
void Scene::setClearColor(float red,float green,float blue) {
    m_options.clearColor[0]=red; m_options.clearColor[1]=green; m_options.clearColor[2]=blue;
    m_changes.grid=true; requestRedraw();
}
void Scene::setAntialiasing(bool enabled) {
    options().antialiasing=enabled && supportsAntialiasing();
    // Both backends already support selecting MSAA during drawing.
    requestRedraw();
}
void Scene::setMipmaps(bool enabled) {
    if(options().mipmaps==enabled) return;
    options().mipmaps=enabled; m_changes.textures=true; requestRedraw();
}

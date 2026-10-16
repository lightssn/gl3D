#include "vkwindow.h"

#if defined(GL3D_HAS_VULKAN)
#include "vkrenderer.h"
#include "meshloader.h"
#include "renderframe.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVector4D>
#include <QWheelEvent>
#include <cmath>
#include <numeric>
#include <QVulkanInstance>
#include <QFileInfo>
#include <QThread>
#include <QMetaObject>
#include <algorithm>
#include <cfloat>

VKWindow::VKWindow(QVulkanInstance* instance, QWindow* parent) : QVulkanWindow(parent) {
    setVulkanInstance(instance);
    m_ready = instance && instance->isValid();
    if (m_ready) {
        const auto counts = supportedSampleCounts();
        const int samples = counts.contains(4) ? 4 : counts.contains(2) ? 2 : 1;
        m_multisamplingAvailable = samples > 1;
        Scene::options().antialiasing = m_multisamplingAvailable;
        if (m_multisamplingAvailable) setSampleCount(samples);
        }
    m_fpsTimer.start();
    m_frameTimer.start();
    m_renderTimer.setTimerType(Qt::PreciseTimer);
    m_idleFpsTimer.setSingleShot(true);
    m_idleFpsTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_idleFpsTimer,&QTimer::timeout,this,[this] {
        if(m_frameTimer.elapsed()>=1000) {
            m_lastFps=0;
            m_frameCount=0;
            emit fpsUpdated(0);
            }
        });
    connect(&m_renderTimer, &QTimer::timeout, this, [this]() {
        if(!Scene::tiled() || Scene::needsTileRedraw()) requestUpdate();
        });
    }

VKWindow::~VKWindow() {
    if (m_loadThread) {
        m_loadThread->quit();
        m_loadThread->wait();
        delete m_loadWorker;
        m_loadWorker = nullptr;
        delete m_loadThread;
        }
    }

bool VKWindow::loadMeshAsync(const QStringList& paths) {
    if (m_loading) return false;
    if (!m_loadThread) {
        qRegisterMetaType<std::shared_ptr<Mesh>>("std::shared_ptr<Mesh>");
        m_loadThread = new QThread(this);
        m_loadWorker = new MeshLoader;
        m_loadWorker->moveToThread(m_loadThread);
        connect(m_loadWorker, &MeshLoader::progress, this, [this](int percent) {
            if (m_loading) emit progressChanged(std::min(90, percent * 9 / 10));
            });
        connect(m_loadWorker, &MeshLoader::finished, this, &VKWindow::onLoadFinished);
        m_loadThread->start();
        }
    m_currentPath = paths.size()==1?paths.front():QStringLiteral("OSGB (%1 files)").arg(paths.size());
    m_loading = true;
    emit loadStarted();
    QMetaObject::invokeMethod(m_loadWorker, "load", Qt::QueuedConnection, Q_ARG(QStringList, paths));
    return true;
    }

void VKWindow::onLoadFinished(bool ok, const QString& error, std::shared_ptr<Mesh> mesh) {
    if (!m_loading) return;
    if (!ok || !mesh) {
        m_loading = false;
        emit loadFailed(error.isEmpty() ? QStringLiteral("加载失败") : error);
        emit loadFinished();
        return;
        }
    Scene::setMesh(std::move(*mesh), m_currentPath);
    if(Scene::tiled()) {
        Scene::options().pbr=false;
        Scene::options().normalMap=false;
        }
    m_pendingPick = QPoint(-1, -1);
    Scene::options().outlineWidth = RenderOptions{}.outlineWidth;
    emit selectionChanged(false);
    emit historyChanged(false, false);
    ++m_modelRevision;
    emit meshLoaded(QString("%1  顶点%2  三角%3  子网格%4").arg(QFileInfo(m_currentPath).fileName()).arg(Scene::mesh().vertexCount()).arg(Scene::mesh().triangleCount()).arg(subMeshCount()));
    requestUpdate();
    emit progressChanged(90);
    }

void VKWindow::meshUploadFinished() {
    if (!m_loading) return;
    m_loading = false;
    emit progressChanged(100);
    emit loadFinished();
    }

void VKWindow::updateTiles() {
    const auto boundsRevision=Scene::tileBoundsRevision();
    if(!Scene::tiled()) return;
    Scene::updateTiles(swapChainImageSize());
    if(boundsRevision!=Scene::tileBoundsRevision()) ++m_gridRevision;
    }

void VKWindow::clearMesh() {
    m_idleFpsTimer.stop();
    ++m_gridRevision;
    Scene::clear();
    m_pendingPick = QPoint(-1, -1);
    emit selectionChanged(false);
    emit historyChanged(false, false);
    ++m_modelRevision;
    requestUpdate();
    }
QVulkanWindowRenderer* VKWindow::createRenderer() {
    return new VKRenderer(this);
    }

namespace {
QVector3D axis(int index)
{
    return EditController::axis(index);
}

QPointF project(const QMatrix4x4& matrix, const QVector3D& point, const QSize& size)
{
    const QVector4D clip = matrix * QVector4D(point, 1.0f);
    if (clip.w() <= 0.0f) return QPointF(1e9, 1e9);
    return QPointF((clip.x() / clip.w() + 1.0f) * size.width() * 0.5f,
                   (1.0f - clip.y() / clip.w()) * size.height() * 0.5f);
}
}

bool VKWindow::takePendingPick(QPoint& position)
{
    if (m_pendingPick.x() < 0) return false;
    position = m_pendingPick;
    m_pendingPick = QPoint(-1, -1);
    return true;
}

float VKWindow::gizmoRadius() const
{
    const Camera& camera = Scene::cameraController().camera();
    const int selected = selectedSubMesh();
    if(selected < 0) return 0;
    const QVector3D center = Scene::selection().centers()[size_t(selected)] + Scene::transformState(selected).position;
    const float depth = -(camera.viewMatrix() * QVector4D(center,1)).z();
    const float halfHeight = camera.ortho() ? camera.distance() * 0.5f
        : std::max(depth,0.000001f) * std::tan(camera.fov() * 0.0087266463f);
    return halfHeight * (2.0f * GizmoPixelRadius) / std::max(1, height());
}

QVector3D VKWindow::gizmoAxis(int index) const
{
    const int selected = selectedSubMesh();
    return EditController::gizmoAxis(index,editor().mode==3 && selected>=0,subMeshRotation(selected));
}

int VKWindow::pickSubMesh(const QPoint& pos) const
{
    if (width() <= 0 || height() <= 0) return -1;
    const Camera& camera = Scene::cameraController().camera();
    const QMatrix4x4 inverse = (camera.projMatrix(float(width()) / height()) * camera.viewMatrix()).inverted();
    const float x = 2.0f * pos.x() / width() - 1.0f;
    const float y = 1.0f - 2.0f * pos.y() / height();
    QVector4D nearPoint = inverse * QVector4D(x, y, -1.0f, 1.0f);
    QVector4D farPoint = inverse * QVector4D(x, y, 1.0f, 1.0f);
    nearPoint /= nearPoint.w();
    farPoint /= farPoint.w();
    const QVector3D rayStart = nearPoint.toVector3D();
    const QVector3D rayEnd = farPoint.toVector3D();
    int hit = -1;
    float closest = FLT_MAX;
    for (int i = 0; i < subMeshCount(); ++i) {
        if (!subMeshVisible(i) || !Scene::drawable(i)) continue;
        const QMatrix4x4 invModel = subMeshTransform(i).inverted();
        const QVector3D origin = invModel.map(rayStart);
        const QVector3D direction = invModel.map(rayEnd) - origin;
        float tMin = 0.0f, tMax = 1.0f;
        const auto& bounds = Scene::mesh().subMeshes[size_t(i)];
        bool valid = true;
        for (int component = 0; component < 3; ++component) {
            const float o = origin[component], d = direction[component];
            if (std::abs(d) < 1e-8f) {
                if (o < bounds.boundsMin[component] || o > bounds.boundsMax[component]) valid = false;
                continue;
            }
            float a = (bounds.boundsMin[component] - o) / d;
            float b = (bounds.boundsMax[component] - o) / d;
            if (a > b) std::swap(a, b);
            tMin = std::max(tMin, a);
            tMax = std::min(tMax, b);
            if (tMin > tMax) valid = false;
        }
        if (valid && tMin < closest) { closest = tMin; hit = i; }
    }
    return hit;
}

int VKWindow::hitGizmo(const QPoint& pos) const
{
    const int index = selectedSubMesh();
    if (!editor().mode || index < 0 || !Scene::options().showGizmo) return 0;
    const QSize size = this->size();
    const Camera& camera = Scene::cameraController().camera();
    const QVector3D center = Scene::selection().centers()[size_t(index)] + Scene::transformState(index).position;
    const QMatrix4x4 gizmo = makeGizmoMatrix(camera.viewMatrix(), camera.projMatrix(float(size.width()) / std::max(1,size.height())), center, size);
    const QPointF start = project(gizmo, {}, size);
    int nearest = 0;
    float nearestDistance = 14.0f;
    for (int i = 1; i <= 3; ++i) {
        if (editor().mode == 2) {
            const QVector3D normal = gizmoAxis(i);
            const QVector3D seed = std::abs(normal.y()) > 0.9f ? QVector3D(1, 0, 0) : QVector3D(0, 1, 0);
            const QVector3D tangent = QVector3D::crossProduct(normal, seed).normalized();
            const QVector3D bitangent = QVector3D::crossProduct(normal, tangent).normalized();
            for (int segment = 0; segment < 64; ++segment) {
                const float a = float(segment) * 6.2831853f / 64.0f;
                const float b = float(segment + 1) * 6.2831853f / 64.0f;
                const QPointF p0 = project(gizmo, tangent * std::cos(a) + bitangent * std::sin(a), size);
                const QPointF p1 = project(gizmo, tangent * std::cos(b) + bitangent * std::sin(b), size);
                const QPointF segmentVector = p1 - p0;
                const float lengthSquared = float(segmentVector.x() * segmentVector.x() + segmentVector.y() * segmentVector.y());
                if (lengthSquared < 1.0f) continue;
                const QPointF relative = QPointF(pos) - p0;
                const float t = std::clamp(float((relative.x() * segmentVector.x() + relative.y() * segmentVector.y()) / lengthSquared), 0.0f, 1.0f);
                const QPointF error = QPointF(pos) - (p0 + segmentVector * t);
                const float distance = float(std::hypot(error.x(), error.y()));
                if (distance < nearestDistance) { nearestDistance = distance; nearest = i; }
            }
            continue;
        }
        const QPointF end = project(gizmo, gizmoAxis(i), size);
        const QPointF vector = end - start;
        const float lengthSquared = float(vector.x() * vector.x() + vector.y() * vector.y());
        if (lengthSquared < 1.0f) continue;
        const QPointF delta = QPointF(pos) - start;
        const float t = std::clamp(float((delta.x() * vector.x() + delta.y() * vector.y()) / lengthSquared), 0.0f, 1.0f);
        const QPointF closest = start + vector * t;
        const QPointF error = QPointF(pos) - closest;
        const float distance = float(std::hypot(error.x(), error.y()));
        if (distance < nearestDistance) { nearestDistance = distance; nearest = i; }
    }
    return nearest;
}

void VKWindow::mousePressEvent(QMouseEvent* event)
{
    m_lastMousePos = event->pos();
    m_dragButton = event->button();
    m_dragDistance = 0;
    m_dragAxis = event->button() == Qt::LeftButton ? hitGizmo(event->pos()) : 0;
    if (m_dragAxis) {
        beginEdit();
        if (editor().mode == 2) {
            const Camera& camera = Scene::cameraController().camera();
            const QMatrix4x4 vp = camera.projMatrix(float(width()) / std::max(1, height())) * camera.viewMatrix();
            const QVector3D center = Scene::selection().centers()[size_t(selectedSubMesh())] + editor().dragStart().position;
            const QPointF relative = QPointF(event->pos()) - project(vp, center, size());
            m_dragAngle = std::atan2(float(relative.y()), float(relative.x()));
        }
    }
    event->accept();
}

void VKWindow::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();
    m_dragDistance += std::abs(delta.x()) + std::abs(delta.y());
    if (m_dragAxis && selectedSubMesh() >= 0) {
        const int index = selectedSubMesh();
        auto& transform = Scene::transformState(index);
        const QVector3D center = Scene::selection().centers()[size_t(index)] + transform.position;
        const Camera& camera = Scene::cameraController().camera();
        const QMatrix4x4 vp = camera.projMatrix(float(width()) / std::max(1, height())) * camera.viewMatrix();
        const float radius = gizmoRadius();
        const QVector3D direction3d = gizmoAxis(m_dragAxis);
        const QMatrix4x4 gizmo = makeGizmoMatrix(camera.viewMatrix(), camera.projMatrix(float(width()) / std::max(1,height())), center, size());
        const QPointF direction = project(gizmo, direction3d, size()) - project(gizmo, {}, size());
        const float pixels = float(std::hypot(direction.x(), direction.y()));
        const float along = pixels > 1.0f ? float((delta.x() * direction.x() + delta.y() * direction.y()) / pixels) : 0.0f;
        if (editor().mode == 1) EditController::translate(transform,direction3d,along*radius/std::max(pixels,1.0f));
        else if (editor().mode == 2) {
            const QPointF relative = QPointF(event->pos()) - project(vp, center, size());
            const float angle = std::atan2(float(relative.y()), float(relative.x()));
            const float difference=EditController::angleDelta(angle,m_dragAngle);
            m_dragAngle = angle;
            transform.rotation = EditController::rotate(transform.rotation,direction3d,difference*57.29578f);
        }
        else if (editor().mode == 3) {
            const float value = EditController::screenScale(transform.scale[m_dragAxis-1],along);
            transform.scale[m_dragAxis - 1] = value;
        }
        Scene::applyTransform(selectedSubMesh());
    } else if (m_dragButton == Qt::LeftButton) {
        Scene::cameraController().camera().orbit(delta.x() * 0.008f, delta.y() * 0.008f);
    } else if (m_dragButton == Qt::RightButton || m_dragButton == Qt::MiddleButton) {
        Scene::cameraController().camera().pan(float(delta.x()), float(delta.y()));
    } else return;
    if (!Scene::options().fixedFps) requestUpdate();
    event->accept();
}

void VKWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragAxis && selectedSubMesh() >= 0) {
        finishEdit(EditController::ChangePolicy::VectorTolerance);
    } else if (event->button() == Qt::LeftButton && m_dragDistance <= 3) {
        m_pendingPick = event->pos();
    } else if (event->button() == Qt::RightButton && m_dragDistance <= 3) {
        emit contextMenuRequested(event->globalPos());
    }
    m_dragAxis = 0;
    m_dragButton = Qt::NoButton;
    requestUpdate();
    event->accept();
}

void VKWindow::wheelEvent(QWheelEvent* event)
{
    Scene::cameraController().camera().zoom(event->angleDelta().y() / 120.0f);
    requestUpdate();
    event->accept();
}

void VKWindow::keyPressEvent(QKeyEvent* event)
{
    Camera& camera = Scene::cameraController().camera();
    switch (event->key()) {
    case Qt::Key_W: camera.panLocal(0, 0, 1); break;
    case Qt::Key_S: camera.panLocal(0, 0, -1); break;
    case Qt::Key_A: camera.panLocal(-1, 0, 0); break;
    case Qt::Key_D: camera.panLocal(1, 0, 0); break;
    case Qt::Key_Q: camera.panLocal(0, -1, 0); break;
    case Qt::Key_E: camera.panLocal(0, 1, 0); break;
    case Qt::Key_R: resetView(); break;
    default: QVulkanWindow::keyPressEvent(event); return;
    }
    requestUpdate();
    event->accept();
}

void VKWindow::recordFrame(int drawCalls)
{
    m_idleFpsTimer.start(1000);
    m_lastDrawCalls = drawCalls;
    const qint64 elapsed = m_frameTimer.restart();
    if (elapsed > 0) {
        m_frameTimes.push_back(float(elapsed));
        if (m_frameTimes.size() > 240) m_frameTimes.erase(m_frameTimes.begin());
    }
    ++m_frameCount;
    if (m_fpsTimer.elapsed() >= 1000) {
        m_lastFps = m_frameCount;
        m_frameCount = 0;
        m_fpsTimer.restart();
        emit fpsUpdated(m_lastFps);
    }
}

void VKWindow::setResourceStats(qint64 vertexBytes, qint64 indexBytes, qint64 textureBytes,
                                    qint64 textureCpuBytes, int textureCount, int bufferCount)
{
    m_vertexBytes = vertexBytes;
    m_indexBytes = indexBytes;
    m_textureBytes = textureBytes;
    m_textureCpuBytes = textureCpuBytes;
    m_textureCount = textureCount;
    m_bufferCount = bufferCount;
}

void VKWindow::setFramebufferStats(qint64 bytes, int ownedCount)
{
    m_framebufferBytes = bytes;
    m_ownedFramebufferCount = ownedCount;
}

void VKWindow::setDeviceDescription(const QString& name, const QString& version)
{
    m_deviceName = name;
    m_apiVersion = version;
}

DebugSnapshot VKWindow::debugSnapshot()
{
    DebugSnapshot snapshot;
    snapshot.fps = m_lastFps;
    snapshot.drawCalls = m_lastDrawCalls;
    snapshot.triangles = Scene::mesh().triangleCount();
    snapshot.frameTimesMs.reserve(int(m_frameTimes.size()));
    for (float sample : m_frameTimes) snapshot.frameTimesMs.append(sample);
    if (!m_frameTimes.empty()) {
        snapshot.averageFrameMs = std::accumulate(m_frameTimes.begin(), m_frameTimes.end(), 0.0f) / m_frameTimes.size();
        snapshot.lowFrameMs = *std::max_element(m_frameTimes.begin(), m_frameTimes.end());
        std::vector<float> sorted = m_frameTimes;
        std::sort(sorted.begin(), sorted.end());
        snapshot.p95FrameMs = sorted[size_t(0.95f * (sorted.size() - 1))];
        snapshot.p99FrameMs = sorted[size_t(0.99f * (sorted.size() - 1))];
        const size_t lowStart = size_t(0.99f * (sorted.size() - 1));
        const float lowAverage = std::accumulate(sorted.begin() + lowStart, sorted.end(), 0.0f) / (sorted.size() - lowStart);
        snapshot.onePercentLowFps = lowAverage > 0.0f ? 1000.0f / lowAverage : 0.0f;
    }
    const Camera& camera = Scene::cameraController().camera();
    snapshot.fov = camera.fov();
    snapshot.distance = camera.distance();
    snapshot.eye = camera.eye();
    snapshot.target = camera.target();
    snapshot.ortho = camera.ortho();
    snapshot.antialiasing = Scene::options().antialiasing;
    snapshot.modelLoaded = Scene::hasMesh();
    snapshot.renderer = m_deviceName;
    snapshot.glVersion = m_apiVersion;
    snapshot.resources = {{"VAO", 0, 0, 0, false}, {"缓冲", m_bufferCount, 0, m_vertexBytes, true},
                          {"EBO", m_indexBytes ? 1 : 0, 0, m_indexBytes, true},
                          {"纹理", m_textureCount, m_textureCpuBytes, m_textureBytes, true},
                          {"FBO", (isValid() ? swapChainImageCount() : 0) + m_ownedFramebufferCount,
                           0, m_framebufferBytes, false}};
    return snapshot;
}

#endif

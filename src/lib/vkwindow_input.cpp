#include "vkwindow.h"
#include "renderframe.h"

#if defined(GL3D_HAS_VULKAN)

#include <QKeyEvent>
#include <QMouseEvent>
#include <QVector4D>
#include <QWheelEvent>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>

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

void VKWindow::resetView()
{
    m_scene.resetView();
    requestUpdate();
}

void VKWindow::setOrtho(bool enabled)
{
    m_scene.cameraController().camera().setOrtho(enabled);
    requestUpdate();
}

void VKWindow::setWireframe(bool enabled) { m_scene.options().wireframe = enabled; ++m_pipelineRevision; requestUpdate(); }
void VKWindow::setAntialiasing(bool enabled) {
    m_scene.options().antialiasing = enabled && m_multisamplingAvailable;
    requestUpdate();
}
void VKWindow::setMipmaps(bool enabled) {
    if (m_mipmaps == enabled) return;
    m_mipmaps = enabled;
    ++m_textureRevision;
    requestUpdate();
}
void VKWindow::setDepthTest(bool enabled) { m_scene.options().depthTest = enabled; ++m_pipelineRevision; requestUpdate(); }
void VKWindow::setFaceCulling(bool enabled) { m_scene.options().faceCulling = enabled; ++m_pipelineRevision; requestUpdate(); }
void VKWindow::setPbr(bool enabled) { m_scene.options().pbr = enabled && supportsPbr(); requestUpdate(); }
void VKWindow::setNormalMap(bool enabled) { m_scene.options().normalMap = enabled && supportsNormalMap(); requestUpdate(); }
void VKWindow::setDebugView(int mode) { m_scene.options().debugView = std::clamp(mode, 0, 2); requestUpdate(); }
void VKWindow::setOutlineWidth(float width) { m_scene.options().outlineWidth = std::max(0.0f, width); requestUpdate(); }

void VKWindow::setFixedFpsEnabled(bool enabled)
{
    m_scene.options().fixedFps = enabled;
    if (enabled) setTargetFps(m_scene.options().targetFps);
    else m_renderTimer.stop();
    requestUpdate();
}

void VKWindow::setTargetFps(int fps)
{
    m_scene.options().targetFps = std::max(0, fps);
    if (m_scene.options().fixedFps)
        m_renderTimer.start(fps > 0 ? std::max(1, 1000 / fps) : 0);
}

void VKWindow::setTransformMode(int mode)
{
    m_transformMode = std::clamp(mode, 0, 3);
    m_dragAxis = 0;
    requestUpdate();
}

QString VKWindow::subMeshName(int index) const
{
    if (index < 0 || index >= subMeshCount()) return {};
    const std::string& name = m_scene.mesh().subMeshes[size_t(index)].materialName;
    return name.empty() ? QStringLiteral("(默认材质)") : QString::fromStdString(name);
}

bool VKWindow::subMeshVisible(int index) const
{
    if(m_scene.tiled()) return m_scene.tileVisible(index);
    return index >= 0 && index < int(m_visible.size()) && m_visible[size_t(index)];
}

void VKWindow::setSubMeshVisible(int index, bool visible)
{
    if(m_scene.tiled()) { m_scene.setTileVisible(index,visible); requestUpdate(); return; }
    if (index < 0 || index >= int(m_visible.size())) return;
    m_visible[size_t(index)] = visible;
    requestUpdate();
}

void VKWindow::selectSubMesh(int index)
{
    if (index < -1 || index >= subMeshCount()) return;
    if (m_scene.tiled() && index >= 0 && !m_scene.mesh().subMeshes[size_t(index)].resident) return;
    if (index == selectedSubMesh()) { emit subMeshSelected(index); return; }
    m_scene.selection().indexRef() = index;
    m_edit.clearHistory();
    emit historyChanged(false, false);
    emit selectionChanged(index >= 0);
    emit subMeshSelected(index);
    requestUpdate();
}

bool VKWindow::takePendingPick(QPoint& position)
{
    if (m_pendingPick.x() < 0) return false;
    position = m_pendingPick;
    m_pendingPick = QPoint(-1, -1);
    return true;
}

QMatrix4x4 VKWindow::subMeshTransform(int index) const
{
    if(m_scene.tiled()) return m_scene.tileTransform(index);
    QMatrix4x4 matrix;
    if (index < 0 || index >= int(m_transforms.size())) return matrix;
    const TransformState& state = m_transforms[size_t(index)];
    const auto& centers = m_scene.selection().centers();
    const QVector3D center = index < int(centers.size()) ? centers[size_t(index)] : m_scene.mesh().center();
    return state.matrix(center);
}

QQuaternion VKWindow::subMeshRotation(int index) const
{
    return index >= 0 && index < int(m_transforms.size())
        ? m_transforms[size_t(index)].rotation : QQuaternion();
}

float VKWindow::gizmoRadius() const
{
    const Camera& camera = m_scene.cameraController().camera();
    const int selected = selectedSubMesh();
    if(selected < 0) return 0;
    const QVector3D center = m_scene.selection().centers()[size_t(selected)] + m_transforms[size_t(selected)].position;
    const float depth = -(camera.viewMatrix() * QVector4D(center,1)).z();
    const float halfHeight = camera.ortho() ? camera.distance() * 0.5f
        : std::max(depth,0.000001f) * std::tan(camera.fov() * 0.0087266463f);
    return halfHeight * (2.0f * GizmoPixelRadius) / std::max(1, height());
}

QVector3D VKWindow::gizmoAxis(int index) const
{
    const int selected = selectedSubMesh();
    return EditController::gizmoAxis(index,m_transformMode==3 && selected>=0,subMeshRotation(selected));
}

void VKWindow::commitTileTransform() {
    const int index=selectedSubMesh();
    if(!m_scene.tiled() || index<0 || index>=int(m_transforms.size())) return;
    const auto& state=m_transforms[size_t(index)];
    const auto center=m_scene.selection().centers()[size_t(index)];
    m_scene.setTileTransform(index,state.matrix(center));
    m_edit.setTileState(m_scene.mesh().subMeshes[size_t(index)].tileOwner,state);
}

void VKWindow::undo() {
    const int index=selectedSubMesh();
    if(index<0 || !m_edit.undo(index,m_transforms[size_t(index)])) return;
    commitTileTransform();
    emit historyChanged(m_edit.canUndo(),m_edit.canRedo());
    requestUpdate();
}

void VKWindow::redo() {
    const int index=selectedSubMesh();
    if(index<0 || !m_edit.redo(index,m_transforms[size_t(index)])) return;
    commitTileTransform();
    emit historyChanged(m_edit.canUndo(),m_edit.canRedo());
    requestUpdate();
}

int VKWindow::pickSubMesh(const QPoint& pos) const
{
    if (width() <= 0 || height() <= 0) return -1;
    const Camera& camera = m_scene.cameraController().camera();
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
    for (int i = 0; i < int(m_bounds.size()); ++i) {
        if (!subMeshVisible(i) || !m_scene.drawable(i)) continue;
        const QMatrix4x4 invModel = subMeshTransform(i).inverted();
        const QVector3D origin = invModel.map(rayStart);
        const QVector3D direction = invModel.map(rayEnd) - origin;
        float tMin = 0.0f, tMax = 1.0f;
        const Bounds& bounds = m_bounds[size_t(i)];
        bool valid = true;
        for (int component = 0; component < 3; ++component) {
            const float o = origin[component], d = direction[component];
            if (std::abs(d) < 1e-8f) {
                if (o < bounds.min[component] || o > bounds.max[component]) valid = false;
                continue;
            }
            float a = (bounds.min[component] - o) / d;
            float b = (bounds.max[component] - o) / d;
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
    if (!m_transformMode || index < 0 || !m_scene.options().showGizmo) return 0;
    const QSize size = this->size();
    const Camera& camera = m_scene.cameraController().camera();
    const QVector3D center = m_scene.selection().centers()[size_t(index)] + m_transforms[size_t(index)].position;
    const QMatrix4x4 gizmo = makeGizmoMatrix(camera.viewMatrix(), camera.projMatrix(float(size.width()) / std::max(1,size.height())), center, size);
    const QPointF start = project(gizmo, {}, size);
    int nearest = 0;
    float nearestDistance = 14.0f;
    for (int i = 1; i <= 3; ++i) {
        if (m_transformMode == 2) {
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
        m_edit.beginDrag(m_transforms[size_t(selectedSubMesh())]);
        if (m_transformMode == 2) {
            const Camera& camera = m_scene.cameraController().camera();
            const QMatrix4x4 vp = camera.projMatrix(float(width()) / std::max(1, height())) * camera.viewMatrix();
            const QVector3D center = m_scene.selection().centers()[size_t(selectedSubMesh())] + m_edit.dragStart().position;
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
        auto& transform = m_transforms[size_t(index)];
        const QVector3D center = m_scene.selection().centers()[size_t(index)] + transform.position;
        const Camera& camera = m_scene.cameraController().camera();
        const QMatrix4x4 vp = camera.projMatrix(float(width()) / std::max(1, height())) * camera.viewMatrix();
        const float radius = gizmoRadius();
        const QVector3D direction3d = gizmoAxis(m_dragAxis);
        const QMatrix4x4 gizmo = makeGizmoMatrix(camera.viewMatrix(), camera.projMatrix(float(width()) / std::max(1,height())), center, size());
        const QPointF direction = project(gizmo, direction3d, size()) - project(gizmo, {}, size());
        const float pixels = float(std::hypot(direction.x(), direction.y()));
        const float along = pixels > 1.0f ? float((delta.x() * direction.x() + delta.y() * direction.y()) / pixels) : 0.0f;
        if (m_transformMode == 1) EditController::translate(transform,direction3d,along*radius/std::max(pixels,1.0f));
        else if (m_transformMode == 2) {
            const QPointF relative = QPointF(event->pos()) - project(vp, center, size());
            const float angle = std::atan2(float(relative.y()), float(relative.x()));
            const float difference=EditController::angleDelta(angle,m_dragAngle);
            m_dragAngle = angle;
            transform.rotation = EditController::rotate(transform.rotation,direction3d,difference*57.29578f);
        }
        else if (m_transformMode == 3) {
            const float value = EditController::screenScale(transform.scale[m_dragAxis-1],along);
            transform.scale[m_dragAxis - 1] = value;
        }
        commitTileTransform();
    } else if (m_dragButton == Qt::LeftButton) {
        m_scene.cameraController().camera().orbit(delta.x() * 0.008f, delta.y() * 0.008f);
    } else if (m_dragButton == Qt::RightButton || m_dragButton == Qt::MiddleButton) {
        m_scene.cameraController().camera().pan(float(delta.x()), float(delta.y()));
    } else return;
    if (!m_scene.options().fixedFps) requestUpdate();
    event->accept();
}

void VKWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragAxis && selectedSubMesh() >= 0) {
        const TransformState& state = m_transforms[size_t(selectedSubMesh())];
        if(m_edit.endDrag(selectedSubMesh(),state,EditController::ChangePolicy::VectorTolerance))
            emit historyChanged(true,false);
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
    m_scene.cameraController().camera().zoom(event->angleDelta().y() / 120.0f);
    requestUpdate();
    event->accept();
}

void VKWindow::keyPressEvent(QKeyEvent* event)
{
    Camera& camera = m_scene.cameraController().camera();
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

void VKWindow::setClearColor(float red, float green, float blue)
{
    m_clearColor[0] = red; m_clearColor[1] = green; m_clearColor[2] = blue;
    ++m_gridRevision;
    requestUpdate();
}

DebugSnapshot VKWindow::debugSnapshot()
{
    DebugSnapshot snapshot;
    snapshot.fps = m_lastFps;
    snapshot.drawCalls = m_lastDrawCalls;
    snapshot.triangles = m_scene.mesh().triangleCount();
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
    const Camera& camera = m_scene.cameraController().camera();
    snapshot.fov = camera.fov();
    snapshot.distance = camera.distance();
    snapshot.eye = camera.eye();
    snapshot.target = camera.target();
    snapshot.ortho = camera.ortho();
    snapshot.antialiasing = m_scene.options().antialiasing;
    snapshot.modelLoaded = m_scene.hasMesh();
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

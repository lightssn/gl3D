#include "osgbstream.h"
#include <QDir>
#include <QFileInfo>
#include <QMatrix4x4>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <chrono>
#include <functional>
#include <unordered_set>

#ifdef GL3D_HAS_OSGB
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/Texture2D>
#include <osgDB/ReadFile>
#include <osgDB/Options>
#include <osgDB/Registry>
#include <QCoreApplication>
#include <QFile>
#include <QBuffer>
#include <QImage>
#endif

OsgbStream::OsgbStream() = default;
OsgbStream::~OsgbStream() { clear(); }
bool OsgbStream::available() const { return m_available; }

void OsgbStream::clear()
{
    // std::future from std::async owns the worker; finish it before destroying tiles.
    for (auto& tile : m_tiles) if (tile.request.valid()) tile.request.wait();
    m_tiles.clear();
    m_visibleMesh = Mesh();
    m_tileMins.clear(); m_tileMaxs.clear(); m_tileIds.clear();
    m_cachedBytes = 0; m_frame = 0; m_available = false;
}

bool OsgbStream::open(const QString& file, QString* error)
{
    clear();
    const QFileInfo source(file);
    if (source.isDir()) {
        QStringList roots;
        const QDir directory(source.absoluteFilePath());
        auto addPackagedRoot = [&](const QDir& folder) {
            const QDir model(folder.absoluteFilePath("Data/Model"));
            for (const QString& name : {"Model_with_transform.osgb", "Model.osgb"}) {
                const QString path = model.absoluteFilePath(name);
                if (QFileInfo(path).isFile()) { roots.push_back(path); return true; }
            }
            return false;
        };
        addPackagedRoot(directory);
        for (const QString& entry : directory.entryList({"*.osgb"}, QDir::Files))
            roots.push_back(directory.absoluteFilePath(entry));
        for (const QString& subdir : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QDir child(directory.absoluteFilePath(subdir));
            if (addPackagedRoot(child)) continue;
            QString rootFile = child.absoluteFilePath(subdir + ".osgb");
            if (!QFileInfo::exists(rootFile)) {
                const QStringList candidates = child.entryList({"*.osgb"}, QDir::Files);
                if (candidates.isEmpty()) continue;
                rootFile = child.absoluteFilePath(candidates.front());
            }
            roots.push_back(rootFile);
        }
        roots.removeDuplicates();
        if (roots.isEmpty()) {
            if (error) *error = QStringLiteral("目录中未找到根 OSGB 瓦片：%1").arg(file);
            return false;
        }
        m_tiles.emplace_back();
        m_tiles[0].path = source.absoluteFilePath();
        m_tiles[0].payload = std::make_shared<Payload>();
        QVector3D minimum(FLT_MAX,FLT_MAX,FLT_MAX), maximum(-FLT_MAX,-FLT_MAX,-FLT_MAX);
        bool first = true;
        QString lastError;
        for (const QString& rootFile : roots) {
            Payload part;
            try { part = readTile(rootFile, m_origin, first); }
            catch (const std::exception& ex) { lastError = QString::fromLocal8Bit(ex.what()); continue; }
            if (!part.error.isEmpty()) { lastError = part.error; continue; }
            if (first) { m_origin = part.origin; first = false; }
            const QVector3D d(part.radius,part.radius,part.radius);
            const QVector3D a = part.center - d, b = part.center + d;
            for (int k=0; k<3; ++k) {
                minimum[k] = std::min(minimum[k],a[k]);
                maximum[k] = std::max(maximum[k],b[k]);
            }
            const int index = int(m_tiles.size());
            m_tiles.emplace_back();
            m_tiles.back().path = rootFile;
            install(index,std::move(part));
            m_tiles[0].children.push_back(index);
        }
        if (first) {
            clear();
            if (error) *error = lastError.isEmpty() ? QStringLiteral("目录里的 OSGB 根瓦片均读取失败：%1").arg(file) : lastError;
            return false;
        }
        m_tiles[0].center = (minimum + maximum)*0.5f;
        m_tiles[0].radius = (maximum-minimum).length()*0.5f;
        m_available = true;
        return true;
    }
    Payload root;
    try { root = readTile(file, {0, 0, 0}, true); }
    catch (const std::exception& ex) { if (error) *error = QString::fromLocal8Bit(ex.what()); return false; }
    if (!root.error.isEmpty()) { if (error) *error = root.error; return false; }
    m_origin = root.origin;
    m_tiles.emplace_back();
    m_tiles[0].path = QFileInfo(file).absoluteFilePath();
    install(0, std::move(root));
    m_available = true;
    rebuild({0});
    return true;
}

void OsgbStream::install(int index, Payload payload)
{
    Tile& tile = m_tiles[size_t(index)];
    tile.center = payload.center; tile.radius = payload.radius;
    tile.children.clear();
    const QString directory = QFileInfo(tile.path).absolutePath();
    for (const Child& child : payload.children) {
        const QString path = QDir::cleanPath(QDir(directory).absoluteFilePath(child.path));
        if (path == tile.path) continue;
        const auto it = std::find_if(m_tiles.begin(), m_tiles.end(), [&](const Tile& t) { return t.path == path; });
        int childIndex;
        if (it == m_tiles.end()) {
            childIndex = int(m_tiles.size());
            m_tiles.emplace_back();
            m_tiles.back().path = path;
            m_tiles.back().center = child.center;
            m_tiles.back().radius = child.radius;
            m_tiles.back().rangeMin = child.rangeMin;
            m_tiles.back().rangeMax = child.rangeMax;
            m_tiles.back().rangeSpecified = child.rangeSpecified;
            m_tiles.back().pixelRange = child.pixelRange;
            m_tiles.back().matrix = child.matrix;
        } else childIndex = int(it - m_tiles.begin());
        m_tiles[size_t(index)].children.push_back(childIndex);
    }
    m_cachedBytes += payload.bytes;
    m_tiles[size_t(index)].payload = std::make_shared<Payload>(std::move(payload));
}

bool OsgbStream::inFrustum(const QVector3D& c, float r) const
{
    for (const auto& p : m_planes)
        if (p[0]*c.x() + p[1]*c.y() + p[2]*c.z() + p[3] < -r) return false;
    return true;
}

void OsgbStream::traverse(int index, const Camera& camera, const QSize& viewport,
                          std::vector<int>& visible, std::vector<int>& requests)
{
    if (index < 0 || index >= int(m_tiles.size())) return;
    Tile& t = m_tiles[size_t(index)];
    if (!inFrustum(t.center, t.radius)) return;
    t.lastUsed = m_frame;
    if (!t.payload) { if (!t.failed && !t.request.valid()) requests.push_back(index); return; }
    const float distance = std::max((camera.eye() - t.center).length() - t.radius, 0.01f);
    const float pixels = camera.ortho()
        ? t.radius * viewport.height() / std::max(camera.distance(), 0.01f)
        : t.radius * viewport.height() / (distance * std::tan(camera.fov() * 0.00872664626f) * 2.0f);
    std::vector<int> desired;
    if (!t.children.empty()) {
        for (int child : t.children) {
            const Tile& c = m_tiles[size_t(child)];
            if (!inFrustum(c.center,c.radius)) continue;
            const float childDistance = (camera.eye()-c.center).length();
            const float childPixels = camera.ortho()
                ? c.radius * viewport.height() * 2.0f / std::max(camera.distance(),0.01f)
                : c.radius * viewport.height() /
                    (std::max(childDistance,0.01f)*std::tan(camera.fov()*0.00872664626f));
            const float rangeValue = c.pixelRange ? childPixels : childDistance;
            const bool active = c.rangeSpecified
                ? (rangeValue >= c.rangeMin && rangeValue < c.rangeMax)
                : (pixels > 120.0f || t.payload->mesh.subMeshes.empty());
            if (active) desired.push_back(child);
        }
        if (desired.empty() && t.payload->mesh.subMeshes.empty())
            for (int child : t.children)
                if (inFrustum(m_tiles[size_t(child)].center,m_tiles[size_t(child)].radius)) desired.push_back(child);
    }
    if (!desired.empty()) {
        bool ready = true;
        for (int child : desired) {
            const Tile& c = m_tiles[size_t(child)];
            if (inFrustum(c.center, c.radius) && !c.payload) {
                if (!c.failed && !c.request.valid()) requests.push_back(child);
                ready = false;
            }
        }
        if (ready) {
            for (int child : desired) traverse(child, camera, viewport, visible, requests);
            return;
        }
    }
    if (!t.payload->mesh.subMeshes.empty()) visible.push_back(index);
}

void OsgbStream::rebuild(const std::vector<int>& visible)
{
    Mesh mesh;
    mesh.bboxMin = QVector3D(FLT_MAX, FLT_MAX, FLT_MAX);
    mesh.bboxMax = QVector3D(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    m_tileMins.clear(); m_tileMaxs.clear(); m_tileIds.clear();
    for (int index : visible) {
        const Payload& p = *m_tiles[size_t(index)].payload;
        for (const SubMesh& sub : p.mesh.subMeshes) {
            mesh.subMeshes.push_back(sub);
            m_tileMins.push_back(p.mesh.bboxMin);
            m_tileMaxs.push_back(p.mesh.bboxMax);
            m_tileIds.push_back(index);
        }
        mesh.bboxMin.setX(std::min(mesh.bboxMin.x(), p.mesh.bboxMin.x()));
        mesh.bboxMin.setY(std::min(mesh.bboxMin.y(), p.mesh.bboxMin.y()));
        mesh.bboxMin.setZ(std::min(mesh.bboxMin.z(), p.mesh.bboxMin.z()));
        mesh.bboxMax.setX(std::max(mesh.bboxMax.x(), p.mesh.bboxMax.x()));
        mesh.bboxMax.setY(std::max(mesh.bboxMax.y(), p.mesh.bboxMax.y()));
        mesh.bboxMax.setZ(std::max(mesh.bboxMax.z(), p.mesh.bboxMax.z()));
    }
    if (mesh.subMeshes.empty()) mesh.bboxMin = mesh.bboxMax = QVector3D();
    m_visibleMesh = std::move(mesh);
}

void OsgbStream::evict(const std::vector<int>& visible)
{
    constexpr qint64 budget = 512LL * 1024 * 1024;
    if (m_cachedBytes <= budget) return;
    std::unordered_set<int> protectedTiles(visible.begin(), visible.end());
    protectedTiles.insert(0);
    while (m_cachedBytes > budget) {
        int victim = -1;
        for (int i = 1; i < int(m_tiles.size()); ++i) {
            const Tile& t = m_tiles[size_t(i)];
            if (t.payload && !protectedTiles.count(i) &&
                (victim < 0 || t.lastUsed < m_tiles[size_t(victim)].lastUsed)) victim = i;
        }
        if (victim < 0) break;
        m_cachedBytes -= m_tiles[size_t(victim)].payload->bytes;
        m_tiles[size_t(victim)].payload.reset();
    }
}

bool OsgbStream::update(const Camera& camera, const QSize& viewport)
{
    if (!m_available || viewport.isEmpty()) return false;
    ++m_frame;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        Tile& t = m_tiles[i];
        if (t.request.valid() && t.request.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                Payload payload = t.request.get();
                if (payload.error.isEmpty()) install(int(i), std::move(payload));
                else t.failed = true;
            } catch (const std::exception&) { t.failed = true; }
        }
    }
    const QMatrix4x4 vp = camera.projMatrix(float(viewport.width()) / viewport.height()) * camera.viewMatrix();
    const float* a = vp.constData();
    for (int i = 0; i < 6; ++i) {
        const int axis = i / 2;
        const float sign = i % 2 ? -1.f : 1.f;
        for (int j = 0; j < 4; ++j) m_planes[i][j] = a[j*4+3] + sign * a[j*4+axis];
        const float n = std::sqrt(m_planes[i][0]*m_planes[i][0] + m_planes[i][1]*m_planes[i][1] + m_planes[i][2]*m_planes[i][2]);
        if (n > 0) for (float& v : m_planes[i]) v /= n;
    }
    std::vector<int> visible, requests;
    traverse(0, camera, viewport, visible, requests);
    std::sort(requests.begin(), requests.end(), [&](int a, int b) {
        return (m_tiles[size_t(a)].center-camera.eye()).lengthSquared() <
               (m_tiles[size_t(b)].center-camera.eye()).lengthSquared();
    });
    int pending = 0;
    for (const Tile& t : m_tiles) pending += t.request.valid();
    for (int i : requests) {
        if (pending >= 2) break;
        Tile& t = m_tiles[size_t(i)];
        if (t.request.valid() || t.failed || t.payload) continue;
        const QString path = t.path;
        const auto origin = m_origin;
        const auto matrix = t.matrix;
        t.request = std::async(std::launch::async, [path, origin, matrix] { return readTile(path, origin, false, matrix); });
        ++pending;
    }
    bool changed = false;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        const bool now = std::find(visible.begin(), visible.end(), int(i)) != visible.end();
        changed |= m_tiles[i].visible != now;
        m_tiles[i].visible = now;
    }
    if (changed) rebuild(visible);
    evict(visible);
    return changed;
}

OsgbStream::Stats OsgbStream::stats() const
{
    Stats s; s.tiles = int(m_tiles.size()); s.cacheBytes = m_cachedBytes;
    for (const Tile& t : m_tiles) { s.visible += t.visible; s.pending += t.request.valid(); s.failed += t.failed; }
    return s;
}

#ifdef GL3D_HAS_OSGB
namespace {
QVector3D convert(const osg::Vec3d& p, const std::array<double, 3>& o)
{
    // OSGB terrain is conventionally Z-up; this viewer's camera uses Y-up.
    return QVector3D(float(p.x()-o[0]), float(p.z()-o[2]), float(-(p.y()-o[1])));
}
QVector3D direction(const osg::Vec3d& p)
{ return QVector3D(float(p.x()), float(p.z()), float(-p.y())); }

void appendGeometry(const osg::Geometry* geometry, const osg::Matrixd& matrix,
                    const std::array<double, 3>& origin, const QString& name, const QString& directory,
                    Mesh& mesh, qint64& bytes)
{
    const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());
    const auto* positionsD = dynamic_cast<const osg::Vec3dArray*>(geometry->getVertexArray());
    const size_t count = positions ? positions->size() : positionsD ? positionsD->size() : 0;
    if (!count) return;
    const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry->getNormalArray());
    const auto* normalsD = dynamic_cast<const osg::Vec3dArray*>(geometry->getNormalArray());
    const auto* uv = dynamic_cast<const osg::Vec2Array*>(geometry->getTexCoordArray(0));
    const auto* uvD = dynamic_cast<const osg::Vec2dArray*>(geometry->getTexCoordArray(0));
    SubMesh sub;
    sub.materialName = name.toStdString();
    sub.vertices.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const osg::Vec3d sourcePosition = positionsD ? (*positionsD)[i]
            : osg::Vec3d((*positions)[i].x(),(*positions)[i].y(),(*positions)[i].z());
        const QVector3D p = convert(sourcePosition * matrix, origin);
        QVector3D n(0, 1, 0);
        if ((normals && !normals->empty()) || (normalsD && !normalsD->empty())) {
            const osg::Vec3d source = normalsD && !normalsD->empty()
                ? (*normalsD)[std::min(i, normalsD->size()-1)]
                : osg::Vec3d((*normals)[std::min(i, normals->size()-1)].x(),
                             (*normals)[std::min(i, normals->size()-1)].y(),
                             (*normals)[std::min(i, normals->size()-1)].z());
            n = direction(osg::Matrixd::transform3x3(source, matrix)).normalized();
        }
        const float u = uv && i < uv->size() ? (*uv)[i].x() : uvD && i < uvD->size() ? float((*uvD)[i].x()) : 0.0f;
        const float v = uv && i < uv->size() ? (*uv)[i].y() : uvD && i < uvD->size() ? float((*uvD)[i].y()) : 0.0f;
        sub.vertices.push_back({p.x(),p.y(),p.z(),n.x(),n.y(),n.z(),u,v});
        mesh.bboxMin.setX(std::min(mesh.bboxMin.x(), p.x()));
        mesh.bboxMin.setY(std::min(mesh.bboxMin.y(), p.y()));
        mesh.bboxMin.setZ(std::min(mesh.bboxMin.z(), p.z()));
        mesh.bboxMax.setX(std::max(mesh.bboxMax.x(), p.x()));
        mesh.bboxMax.setY(std::max(mesh.bboxMax.y(), p.y()));
        mesh.bboxMax.setZ(std::max(mesh.bboxMax.z(), p.z()));
    }
    auto triangle = [&](unsigned a, unsigned b, unsigned c) {
        if (a >= sub.vertices.size() || b >= sub.vertices.size() || c >= sub.vertices.size()) return;
        sub.indices.insert(sub.indices.end(), {a,b,c}); // The axis conversion is a rotation, so winding is preserved.
    };
    for (unsigned ps = 0; ps < geometry->getNumPrimitiveSets(); ++ps) {
        const osg::PrimitiveSet* primitive = geometry->getPrimitiveSet(ps);
        const unsigned count = primitive->getNumIndices();
        const GLenum mode = primitive->getMode();
        if (mode == GL_TRIANGLES) for (unsigned i=0; i+2<count; i+=3) triangle(primitive->index(i),primitive->index(i+1),primitive->index(i+2));
        else if (mode == GL_TRIANGLE_STRIP) for (unsigned i=0; i+2<count; ++i) {
            if (i & 1) triangle(primitive->index(i+1),primitive->index(i),primitive->index(i+2));
            else triangle(primitive->index(i),primitive->index(i+1),primitive->index(i+2));
        }
        else if (mode == GL_TRIANGLE_FAN || mode == GL_POLYGON) for (unsigned i=1; i+1<count; ++i) triangle(primitive->index(0),primitive->index(i),primitive->index(i+1));
        else if (mode == GL_QUADS) for (unsigned i=0; i+3<count; i+=4) {
            triangle(primitive->index(i),primitive->index(i+1),primitive->index(i+2));
            triangle(primitive->index(i),primitive->index(i+2),primitive->index(i+3));
        }
    }
    if (sub.indices.empty()) return;
    if ((!normals || normals->empty()) && (!normalsD || normalsD->empty())) {
        std::vector<QVector3D> generated(sub.vertices.size());
        for (size_t i=0; i+2<sub.indices.size(); i+=3) {
            const Vertex& a = sub.vertices[sub.indices[i]];
            const Vertex& b = sub.vertices[sub.indices[i+1]];
            const Vertex& c = sub.vertices[sub.indices[i+2]];
            const QVector3D pa(a.px,a.py,a.pz), pb(b.px,b.py,b.pz), pc(c.px,c.py,c.pz);
            const QVector3D normal = QVector3D::crossProduct(pb-pa,pc-pa);
            for (int j=0; j<3; ++j) generated[sub.indices[i+j]] += normal;
        }
        for (size_t i=0; i<sub.vertices.size(); ++i) {
            const QVector3D normal = generated[i].lengthSquared() > 0 ? generated[i].normalized() : QVector3D(0,1,0);
            sub.vertices[i].nx = normal.x(); sub.vertices[i].ny = normal.y(); sub.vertices[i].nz = normal.z();
        }
    }
    const osg::StateSet* state = geometry->getStateSet();
    if (state) {
        if (const auto* material = dynamic_cast<const osg::Material*>(state->getAttribute(osg::StateAttribute::MATERIAL))) {
            const osg::Vec4 color = material->getDiffuse(osg::Material::FRONT);
            sub.diffuseColor = QVector3D(color.r(),color.g(),color.b());
        }
        if (const auto* texture = dynamic_cast<const osg::Texture2D*>(state->getTextureAttribute(0, osg::StateAttribute::TEXTURE))) {
            const osg::Image* image = texture->getImage();
            if (image && image->valid() && !image->isCompressed() &&
                image->s() <= 16384 && image->t() <= 16384) {
                sub.flipTextureVertically = image->getOrigin() == osg::Image::BOTTOM_LEFT;
                QImage converted(image->s(), image->t(), QImage::Format_RGBA8888);
                for (int y=0; y<image->t(); ++y) for (int x=0; x<image->s(); ++x) {
                    const osg::Vec4 c = image->getColor(x,y);
                    converted.setPixelColor(x,y,QColor::fromRgbF(c.r(),c.g(),c.b(),c.a()));
                }
                QBuffer buffer(&sub.textureData); buffer.open(QIODevice::WriteOnly); converted.save(&buffer,"PNG");
            } else if (image && !image->getFileName().empty()) {
                sub.texturePath = QDir(directory).absoluteFilePath(QString::fromStdString(image->getFileName()));
            }
        }
    }
    bytes += qint64(sub.vertices.size()*sizeof(Vertex) + sub.indices.size()*sizeof(unsigned) + sub.textureData.size());
    mesh.subMeshes.push_back(std::move(sub));
}
}

OsgbStream::Payload OsgbStream::readTile(const QString& path, const std::array<double, 3>& inputOrigin,
                                         bool root, const MatrixArray& parentMatrix)
{
    Payload out;
    if (root) {
        auto& paths = osgDB::Registry::instance()->getLibraryFilePathList();
        const QString local = QCoreApplication::applicationDirPath() + "/osgPlugins-3.6.5";
        if (QDir(local).exists()) paths.push_back(local.toUtf8().toStdString());
#ifdef _DEBUG
#ifdef GL3D_OSG_PLUGIN_DEBUG
        paths.push_back(GL3D_OSG_PLUGIN_DEBUG);
#endif
#else
#ifdef GL3D_OSG_PLUGIN_RELEASE
        paths.push_back(GL3D_OSG_PLUGIN_RELEASE);
#endif
#endif
    }
    osg::ref_ptr<osgDB::Options> options = new osgDB::Options;
    options->setObjectCacheHint(osgDB::Options::CACHE_NONE);
    options->setBuildKdTreesHint(osgDB::Options::DO_NOT_BUILD_KDTREES);
    const osg::ref_ptr<osg::Node> node = osgDB::readNodeFile(path.toUtf8().toStdString(),options.get());
    if (!node) { out.error = QStringLiteral("无法读取 OSGB 瓦片：%1（请检查 osgdb_osgb 插件和文件路径）").arg(path); return out; }
    const osg::BoundingSphere sphere = node->getBound();
    const osg::Matrixd initial(parentMatrix.data());
    const osg::Vec3d sphereCenter = osg::Vec3d(sphere.center().x(),sphere.center().y(),sphere.center().z()) * initial;
    out.origin = root ? std::array<double,3>{sphereCenter.x(),sphereCenter.y(),sphereCenter.z()} : inputOrigin;
    out.center = convert(sphereCenter, out.origin);
    const osg::Vec3d initialScale = initial.getScale();
    out.radius = float(sphere.radius() * std::max({initialScale.x(),initialScale.y(),initialScale.z()}));
    out.mesh.bboxMin = QVector3D(FLT_MAX,FLT_MAX,FLT_MAX);
    out.mesh.bboxMax = QVector3D(-FLT_MAX,-FLT_MAX,-FLT_MAX);
    const QString directory = QFileInfo(path).absolutePath();
    std::function<void(const osg::Node*, const osg::Matrixd&, const osg::StateSet*)> visit;
    visit = [&](const osg::Node* current, const osg::Matrixd& parent, const osg::StateSet* inherited) {
        const osg::Matrixd matrix = dynamic_cast<const osg::MatrixTransform*>(current)
            ? dynamic_cast<const osg::MatrixTransform*>(current)->getMatrix() * parent : parent;
        const osg::StateSet* state = current->getStateSet() ? current->getStateSet() : inherited;
        if (const auto* geode = current->asGeode()) {
            for (unsigned i=0; i<geode->getNumDrawables(); ++i) {
                const osg::Drawable* drawable = geode->getDrawable(i);
                const osg::Geometry* geometry = drawable->asGeometry();
                if (!geometry) continue;
                // State inherited from a parent node is copied only when the drawable has none.
                osg::ref_ptr<osg::Geometry> copy;
                if (!geometry->getStateSet() && state) {
                    copy = static_cast<osg::Geometry*>(geometry->clone(osg::CopyOp::SHALLOW_COPY));
                    copy->setStateSet(const_cast<osg::StateSet*>(state));
                    geometry = copy.get();
                }
                const QString tileName = QFileInfo(path).fileName();
                const QString drawableName = QString::fromStdString(current->getName());
                appendGeometry(geometry,matrix,out.origin,
                               drawableName.isEmpty() ? tileName : tileName + " / " + drawableName,
                               directory,out.mesh,out.bytes);
            }
        }
        if (const auto* paged = dynamic_cast<const osg::PagedLOD*>(current)) {
            for (unsigned i=0; i<std::min(paged->getNumRanges(),paged->getNumFileNames()); ++i) {
                const std::string file = paged->getFileName(i);
                if (file.empty()) continue;
                QString childPath = QString::fromStdString(file);
                const QString db = QString::fromStdString(paged->getDatabasePath());
                if (!db.isEmpty()) childPath = QDir(db).filePath(childPath);
                const QString absolute = QDir(directory).absoluteFilePath(childPath);
                const auto center = paged->getCenter();
                const osg::Vec3d c = osg::Vec3d(center.x(),center.y(),center.z()) * matrix;
                Child child;
                child.path = QDir(directory).relativeFilePath(absolute);
                child.center = convert(c,out.origin);
                const osg::Vec3d scale = matrix.getScale();
                child.radius = float(std::max(0.0, double(paged->getRadius())) *
                    std::max({scale.x(),scale.y(),scale.z()}));
                child.rangeMin = paged->getMinRange(i);
                child.rangeMax = paged->getMaxRange(i);
                child.rangeSpecified = true;
                child.pixelRange = paged->getRangeMode() == osg::LOD::PIXEL_SIZE_ON_SCREEN;
                std::copy(matrix.ptr(), matrix.ptr()+16, child.matrix.begin());
                out.children.push_back(std::move(child));
            }
        }
        if (const auto* group = current->asGroup()) for (unsigned i=0; i<group->getNumChildren(); ++i)
            visit(group->getChild(i),matrix,state);
    };
    visit(node.get(),initial,nullptr);
    if (out.mesh.subMeshes.empty() && out.children.empty())
        out.error = QStringLiteral("OSGB 中没有可渲染的三角形或 PagedLOD 子瓦片：%1").arg(path);
    if (out.mesh.subMeshes.empty()) out.mesh.bboxMin = out.mesh.bboxMax = out.center;
    return out;
}
#else
OsgbStream::Payload OsgbStream::readTile(const QString&, const std::array<double,3>&, bool, const MatrixArray&)
{
    Payload out; out.error = QStringLiteral("当前构建未启用 OSGB：请用 vcpkg 安装 osg:x64-windows 后重新配置 CMake"); return out;
}
#endif

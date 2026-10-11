#include "osgbloader.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QBuffer>
#include <QDebug>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Transform>
#include <osg/PagedLOD>
#include <osg/Material>
#include <osg/Texture2D>
#include <osg/TriangleIndexFunctor>
#include <osg/Switch>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <functional>
#include <set>

namespace {
std::once_flag pluginPaths;
struct Triangles {
    std::vector<unsigned int>* out = nullptr;
    void operator()(unsigned int a, unsigned int b, unsigned int c) {
        if (a != b && b != c && a != c) out->insert(out->end(), {a, b, c});
    }
};
QVector3D qvec(const osg::Vec3d& p) { return {float(p.x()), float(p.y()), float(p.z())}; }

// OSG 读取的嵌入图片保持原始 UV 朝向。DDS 在后台解码，两个后端共享结果。
QImage imagePixels(const osg::Image& image) {
    if (image.s() <= 0 || image.t() <= 0 || !image.data()) return {};
    const auto format = image.getPixelFormat();
    constexpr unsigned DXT1 = 0x83F0, DXT1A = 0x83F1, DXT3 = 0x83F2, DXT5 = 0x83F3;
    QImage output(image.s(), image.t(), QImage::Format_RGBA8888);
    if (format >= DXT1 && format <= DXT5) {
        const int blockBytes = format <= DXT1A ? 8 : 16;
        const int bxCount = (image.s() + 3) / 4, byCount = (image.t() + 3) / 4;
        if (size_t(bxCount) * byCount * blockBytes > image.getTotalSizeInBytes()) return {};
        auto u16 = [](const unsigned char* p) { return unsigned(p[0]) | (unsigned(p[1]) << 8); };
        auto rgb = [](unsigned v) { return QColor(((v >> 11) & 31) * 255 / 31,
            ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31); };
        for (int by = 0; by < byCount; ++by) for (int bx = 0; bx < bxCount; ++bx) {
            const unsigned char* block = image.data() + (by * bxCount + bx) * blockBytes;
            const unsigned char* colors = block + (blockBytes == 16 ? 8 : 0);
            const unsigned c0 = u16(colors), c1 = u16(colors + 2);
            QColor palette[4]{rgb(c0), rgb(c1), QColor(), QColor()};
            const bool four = c0 > c1 || blockBytes == 16 || format == DXT1;
            for (int channel = 0; channel < 3; ++channel) {
                const int a = channel == 0 ? palette[0].red() : channel == 1 ? palette[0].green() : palette[0].blue();
                const int b = channel == 0 ? palette[1].red() : channel == 1 ? palette[1].green() : palette[1].blue();
                auto set = [channel](QColor& c, int v) { if (!channel) c.setRed(v); else if (channel == 1) c.setGreen(v); else c.setBlue(v); };
                set(palette[2], four ? (2*a+b)/3 : (a+b)/2);
                set(palette[3], four ? (a+2*b)/3 : 0);
            }
            palette[3].setAlpha(four ? 255 : 0);
            uint32_t codes = 0; std::memcpy(&codes, colors + 4, 4);
            unsigned alpha[8]{block[0], block[1], 0, 0, 0, 0, 0, 255};
            uint64_t alphaBits = 0;
            if (format == DXT5) {
                for (int k = 0; k < 6; ++k) alphaBits |= uint64_t(block[2+k]) << (8*k);
                const int n = alpha[0] > alpha[1] ? 7 : 5;
                for (int k = 1; k < n; ++k) alpha[k+1] = ((n-k)*alpha[0]+k*alpha[1])/n;
            }
            for (int p = 0; p < 16; ++p) {
                const int x = bx*4+p%4, y = by*4+p/4;
                if (x >= output.width() || y >= output.height()) continue;
                QColor c = palette[(codes >> (2*p)) & 3];
                if (format == DXT3) c.setAlpha(((block[p/2] >> ((p%2)*4)) & 15)*17);
                if (format == DXT5) c.setAlpha(alpha[(alphaBits >> (3*p)) & 7]);
                output.setPixelColor(x, y, c);
            }
        }
    } else {
        if (image.isCompressed()) return {};
        for (int y = 0; y < image.t(); ++y) for (int x = 0; x < image.s(); ++x) {
            const osg::Vec4 c = image.getColor(x, y);
            output.setPixelColor(x, y, QColor::fromRgbF(std::clamp(c.r(), 0.f, 1.f),
                std::clamp(c.g(), 0.f, 1.f), std::clamp(c.b(), 0.f, 1.f), std::clamp(c.a(), 0.f, 1.f)));
        }
    }
    return output;
}

// Count known OSG payloads once; serializer and allocator overhead remain untracked.
struct SourceMemory : osg::NodeVisitor {
    qint64 bytes=0;
    std::set<const void*> seen;
    SourceMemory():NodeVisitor(TRAVERSE_ALL_CHILDREN) { setNodeMaskOverride(~0u); }
    void state(const osg::StateSet* state) {
        if(!state) return;
        for(unsigned unit=0;unit<state->getTextureAttributeList().size();++unit) {
            auto* texture=dynamic_cast<const osg::Texture2D*>(state->getTextureAttribute(unit,osg::StateAttribute::TEXTURE));
            if(texture && texture->getImage() && seen.insert(texture->getImage()).second) bytes+=texture->getImage()->getTotalSizeInBytes();
        }
    }
    void array(const osg::Array* data) { if(data && seen.insert(data).second) bytes+=data->getTotalDataSize(); }
    void apply(osg::Node& node) override { state(node.getStateSet()); traverse(node); }
    void apply(osg::Geode& node) override {
        state(node.getStateSet());
        for(unsigned i=0;i<node.getNumDrawables();++i) {
            auto* geometry=dynamic_cast<osg::Geometry*>(node.getDrawable(i));
            if(!geometry || !seen.insert(geometry).second) continue;
            state(geometry->getStateSet()); array(geometry->getVertexArray()); array(geometry->getNormalArray()); array(geometry->getColorArray());
            for(auto& uv:geometry->getTexCoordArrayList()) array(uv.get());
            for(auto& primitive:geometry->getPrimitiveSetList()) if(seen.insert(primitive.get()).second) bytes+=primitive->getTotalDataSize();
        }
        traverse(node);
    }
};

struct Reader {
    osg::Vec3d origin;
    QString directory;
    int ownerCounter = 0;
    bool unsupported = false;
    std::shared_ptr<std::atomic_bool> cancel;
    std::shared_ptr<PageMemory> memory;
    std::map<const osg::Image*, QImage> decodedImages;
    QVector3D inheritedDiffuse{0.7f, 0.7f, 0.7f};
    QImage inheritedImage;
    void readMaterial(SubMesh& mesh, const osg::StateSet* state) {
        mesh.diffuseColor = inheritedDiffuse;
        mesh.baseImage = inheritedImage;
    if (auto* material = dynamic_cast<const osg::Material*>(state->getAttribute(osg::StateAttribute::MATERIAL))) {
        const auto c=material->getDiffuse(osg::Material::FRONT); mesh.diffuseColor={c.r(),c.g(),c.b()};
        if (c.a()<1.f) unsupported=true;
    }
    if (auto* texture = dynamic_cast<const osg::Texture2D*>(state->getTextureAttribute(0, osg::StateAttribute::TEXTURE))) {
        if (const auto* image=texture->getImage()) {
            auto found=decodedImages.find(image);
            QImage decoded=found==decodedImages.end()?imagePixels(*image):found->second;
            if(found==decodedImages.end()) { decodedImages.emplace(image,decoded); if(memory) memory->convertedBytes.fetch_add(decoded.sizeInBytes()); }
            if (decoded.isNull()) { unsupported=true; qWarning() << "Unsupported OSGB image" << image->getPixelFormat(); }
            else mesh.baseImage=decoded;
        }
    }
    }
    std::shared_ptr<TileNode> convert(osg::Node& node, osg::Matrixd world,
                                     const osg::StateSet* inherited, int owner) {
        if(cancel && cancel->load()) return {};
        if (auto* transform = dynamic_cast<osg::Transform*>(&node))
            transform->computeLocalToWorldMatrix(world, nullptr);
        osg::ref_ptr<osg::StateSet> state = inherited ? new osg::StateSet(*inherited, osg::CopyOp::SHALLOW_COPY) : new osg::StateSet;
        if (node.getStateSet()) state->merge(*node.getStateSet());
        if (state->getAttribute(osg::StateAttribute::PROGRAM) || (state->getMode(GL_BLEND) & osg::StateAttribute::ON)) unsupported = true;
        auto result = std::make_shared<TileNode>();
        if(memory) memory->convertedBytes.fetch_add(sizeof(TileNode));
        result->name = QString::fromStdString(node.getName());
        result->owner = owner;
        std::copy(world.ptr(), world.ptr()+16, result->sourceTransform.begin());
        result->origin = {origin.x(), origin.y(), origin.z()};
        const auto bound = node.getBound();
        // Transform 的 getBound 已在父坐标系，不能重复应用自身矩阵。
        osg::Matrixd boundMatrix = world;
        if (auto* transform = dynamic_cast<osg::Transform*>(&node)) {
            osg::Matrixd inverse; transform->computeWorldToLocalMatrix(inverse, nullptr);
            boundMatrix = inverse * world;
        }
        result->center = qvec(osg::Vec3d(bound.center()) * boundMatrix - origin);
        const double scale = std::max({osg::Matrixd::transform3x3(osg::Vec3d(1,0,0), boundMatrix).length(),
            osg::Matrixd::transform3x3(osg::Vec3d(0,1,0), boundMatrix).length(),
            osg::Matrixd::transform3x3(osg::Vec3d(0,0,1), boundMatrix).length()});
        result->radius = float(std::max(0.0, double(bound.radius()) * scale));
        if (auto* geode = dynamic_cast<osg::Geode*>(&node)) {
            for (unsigned d = 0; d < geode->getNumDrawables(); ++d) {
                if(cancel && cancel->load()) return {};
                auto* geometry = dynamic_cast<osg::Geometry*>(geode->getDrawable(d));
                if (!geometry) { unsupported = true; continue; }
                auto* positions = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
                auto* positionsD = dynamic_cast<osg::Vec3dArray*>(geometry->getVertexArray());
                if (!positions && !positionsD) { unsupported = true; continue; }
                SubMesh mesh;
                mesh.tileOwner = owner;
                mesh.flipTextureVertically = false;
                osg::TriangleIndexFunctor<Triangles> triangles; triangles.out = &mesh.indices;
                geometry->accept(triangles);
                if (mesh.indices.empty()) continue;
                const size_t count = positions ? positions->size() : positionsD->size();
                if (std::any_of(mesh.indices.begin(), mesh.indices.end(), [count](unsigned i) { return i >= count; })) {
                    unsupported = true; continue;
                }
                mesh.vertices.resize(count);
                if(memory) memory->convertedBytes.fetch_add(qint64(mesh.vertices.capacity()*sizeof(Vertex)+mesh.indices.capacity()*sizeof(unsigned)));
                auto* uv = dynamic_cast<osg::Vec2Array*>(geometry->getTexCoordArray(0));
                auto* normals = dynamic_cast<osg::Vec3Array*>(geometry->getNormalArray());
                osg::Matrixd normalMatrix; normalMatrix.transpose(osg::Matrixd::inverse(world));
                for (size_t v = 0; v < count; ++v) {
                    const osg::Vec3d p = (positions ? osg::Vec3d((*positions)[v]) : (*positionsD)[v]) * world - origin;
                    QVector3D n;
                    if (normals && !normals->empty()) {
                        const size_t ni = geometry->getNormalBinding() == osg::Geometry::BIND_OVERALL ? 0 : v;
                        if (ni < normals->size()) n = qvec(osg::Matrixd::transform3x3(osg::Vec3d((*normals)[ni]), normalMatrix)).normalized();
                    }
                    const osg::Vec2 t = uv && v < uv->size() ? (*uv)[v] : osg::Vec2();
                    mesh.vertices[v] = {float(p.x()), float(p.y()), float(p.z()), n.x(), n.y(), n.z(), t.x(), t.y()};
                    const QVector3D qp = qvec(p);
                    if (!v) mesh.boundsMin = mesh.boundsMax = qp;
                    else for (int k=0;k<3;++k) { mesh.boundsMin[k]=std::min(mesh.boundsMin[k],qp[k]); mesh.boundsMax[k]=std::max(mesh.boundsMax[k],qp[k]); }
                }
                if (!normals || geometry->getNormalBinding() != osg::Geometry::BIND_PER_VERTEX) {
                    std::vector<QVector3D> sums(count);
                    auto position = [&](unsigned i) { const auto& v=mesh.vertices[i]; return QVector3D(v.px,v.py,v.pz); };
                    for (size_t i=0;i<mesh.indices.size();i+=3) {
                        const auto a=mesh.indices[i], b=mesh.indices[i+1], c=mesh.indices[i+2];
                        const auto n=QVector3D::crossProduct(position(b)-position(a),position(c)-position(a));
                        sums[a]+=n; sums[b]+=n; sums[c]+=n;
                    }
                    for (size_t i=0;i<count;++i) { auto n=sums[i].normalized(); mesh.vertices[i].nx=n.x(); mesh.vertices[i].ny=n.y(); mesh.vertices[i].nz=n.z(); }
                }
                osg::ref_ptr<osg::StateSet> materialState = new osg::StateSet(*state, osg::CopyOp::SHALLOW_COPY);
                if (geometry->getStateSet()) materialState->merge(*geometry->getStateSet());
                readMaterial(mesh, materialState);
                if (geometry->getColorArray() || materialState->getAttribute(osg::StateAttribute::PROGRAM)) unsupported=true;
                mesh.materialName=(result->name.isEmpty()?QStringLiteral("OSGB"):result->name).toStdString();
                // 只合并同一节点内相同材质的小几何，避免破坏分页和编辑边界。
                auto same=[&](const SubMesh& s) { return s.baseImage==mesh.baseImage && s.diffuseColor==mesh.diffuseColor && s.vertices.size()+mesh.vertices.size()<=250000; };
                auto found=std::find_if(result->geometry.begin(),result->geometry.end(),same);
                if (found==result->geometry.end()) result->geometry.push_back(std::move(mesh));
                else {
                    const unsigned offset=unsigned(found->vertices.size());
                    found->vertices.insert(found->vertices.end(),mesh.vertices.begin(),mesh.vertices.end());
                    for (auto i:mesh.indices) found->indices.push_back(i+offset);
                    for(int k=0;k<3;++k) { found->boundsMin[k]=std::min(found->boundsMin[k],mesh.boundsMin[k]); found->boundsMax[k]=std::max(found->boundsMax[k],mesh.boundsMax[k]); }
                }
            }
        }
        auto* group=node.asGroup();
        auto* lod=dynamic_cast<osg::LOD*>(&node);
        auto* paged=dynamic_cast<osg::PagedLOD*>(&node);
        result->lod=lod!=nullptr;
        result->pixelRange=lod && lod->getRangeMode()==osg::LOD::PIXEL_SIZE_ON_SCREEN;
        if (lod) result->center=qvec(osg::Vec3d(lod->getCenter())*world-origin);
        const unsigned children=group?group->getNumChildren():0;
        const unsigned total=paged?std::max(children,paged->getNumFileNames()):children;
        for(unsigned i=0;i<total;++i) {
            if (auto* sw=dynamic_cast<osg::Switch*>(&node)) if (i<children && !sw->getValue(i)) continue;
            const int childOwner=owner>=0?owner:ownerCounter++;
            std::shared_ptr<TileNode> child;
            if(i<children) child=convert(*group->getChild(i),world,state,childOwner);
            else child=std::make_shared<TileNode>();
            if(!child) return {};
            child->owner=childOwner;
            if(lod && i<lod->getNumRanges()) { child->minRange=lod->getMinRange(i); child->maxRange=lod->getMaxRange(i); }
            if(paged && !paged->getFileName(i).empty()) {
                QDir base(directory);
                if(!paged->getDatabasePath().empty()) base=QDir(base.filePath(QString::fromStdString(paged->getDatabasePath())));
                child->file=QDir::cleanPath(base.filePath(QString::fromStdString(paged->getFileName(i))));
                child->external=true; child->loaded=i<children;
                SubMesh inherited; readMaterial(inherited, state);
                child->inheritedDiffuse = inherited.diffuseColor;
                child->inheritedImage = inherited.baseImage;
                child->name=QFileInfo(child->file).fileName();
                std::copy(world.ptr(),world.ptr()+16,child->sourceTransform.begin());
                child->origin=result->origin;
                if(i>=children) { child->center=result->center; child->radius=result->radius; }
            }
            if(child) result->children.push_back(std::move(child));
        }
        return result;
    }
};

osg::ref_ptr<osg::Node> read(const QString& path, QString* error) {
    std::call_once(pluginPaths, [] {
        auto& paths=osgDB::Registry::instance()->getLibraryFilePathList();
        paths.push_back((QCoreApplication::applicationDirPath()+"/osgPlugins-3.6.5").toStdString());
    });
    auto node=osgDB::readRefNodeFile(path.toUtf8().toStdString());
    if(!node && error) *error=QStringLiteral("无法读取 OSGB：%1；检查文件及 OSG 插件部署").arg(path);
    return node;
}
}

std::shared_ptr<TileNode> OsgbLoader::readPage(const QString& path,
    const std::array<double,16>& transform,const std::array<double,3>& origin,QString* error,
    std::shared_ptr<std::atomic_bool> cancel,
    const QVector3D& inheritedDiffuse, const QImage& inheritedImage, std::shared_ptr<PageMemory> memory) {
    if(cancel && cancel->load()) return {};
    if(memory) memory->reading=true;
    auto node=read(path,error);
    if(memory) memory->reading=false;
    if(!node || (cancel && cancel->load())) return {};
    if(memory) { SourceMemory source; node->accept(source); memory->sourceBytes=source.bytes; }
    Reader reader; reader.memory=memory; reader.inheritedDiffuse=inheritedDiffuse; reader.inheritedImage=inheritedImage; reader.cancel=cancel; reader.origin.set(origin[0],origin[1],origin[2]); reader.directory=QFileInfo(path).absolutePath();
    auto root=reader.convert(*node,osg::Matrixd(transform.data()),nullptr,0);
    if(reader.unsupported) qWarning() << "OSGB page contains unsupported custom state:" << path;
    return root;
}

bool OsgbLoader::load(const QString& path, Mesh& mesh, QString* error) {
    return loadMany({path}, mesh, error);
}

bool OsgbLoader::loadMany(const QStringList& paths, Mesh& mesh, QString* error,
    const std::function<void(int)>& progress) {
    if (paths.isEmpty()) { if(error) *error="No OSGB files selected"; return false; }
    std::vector<osg::ref_ptr<osg::Node>> nodes;
    osg::Vec3d minimum, maximum;
    for (const auto& path : paths) {
        auto node=read(path,error); if(!node) return false;
        const auto bound=node->getBound();
        const osg::Vec3d center(bound.center());
        const double radius=std::max(0.0,double(bound.radius()));
        for (int k=0;k<3;++k) {
            minimum[k]=nodes.empty()?center[k]-radius:std::min(minimum[k],center[k]-radius);
            maximum[k]=nodes.empty()?center[k]+radius:std::max(maximum[k],center[k]+radius);
        }
        nodes.push_back(node);
        if(progress) progress(int(nodes.size()*50/paths.size()));
    }
    Reader reader; reader.origin=(minimum+maximum)*0.5;
    auto aggregate=std::make_shared<TileNode>();
    aggregate->name=QStringLiteral("OSGB (%1 files)").arg(paths.size());
    aggregate->radius=float((maximum-minimum).length()*0.5);
    for (qsizetype i=0;i<paths.size();++i) {
        const auto& path=paths[i]; const auto& node=nodes[size_t(i)];
        reader.directory=QFileInfo(path).absolutePath();
        reader.decodedImages.clear();
        const int owner=dynamic_cast<osg::LOD*>(node.get()) || dynamic_cast<osg::Geode*>(node.get())
            ? reader.ownerCounter++ : -1;
        auto root=reader.convert(*node,osg::Matrixd::identity(),nullptr,owner);
        if(!root || (root->geometry.empty() && root->children.empty())) {
            if(error) *error=QStringLiteral("OSGB contains no supported mesh or pages: %1").arg(path);
            return false;
        }
        root->name=QFileInfo(path).fileName();
        std::function<void(const std::shared_ptr<TileNode>&)> traceRoot=[&](const auto& n) {
            n->ancestry.append(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
            for(auto& child:n->children) traceRoot(child);
        };
        traceRoot(root);
        aggregate->children.push_back(root);
        if(progress) progress(50+int((i+1)*50/paths.size()));
    }
    Mesh result;
    result.tileRoot=paths.size()==1?aggregate->children.front():aggregate;
    const auto& root=result.tileRoot;
    const QVector3D extent(root->radius,root->radius,root->radius);
    result.bboxMin=root->center-extent; result.bboxMax=root->center+extent;
    mesh=std::move(result);
    if(reader.unsupported) qWarning()<<"OSGB: custom shader/vertex color/transparency is not reproduced";
    return true;
}

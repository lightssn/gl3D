#include "tilestream.h"
#if defined(GL3D_HAS_OSGB)
#include "osgbloader.h"
#endif
#include <QDebug>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <set>
#include <QThreadPool>

TileStream::TileStream(Mesh& mesh) : m_root(mesh.tileRoot) {
    m_clock.start();
    discover(m_root,0);
    install(mesh, m_root, -1);
}
TileStream::~TileStream() { m_cancel->store(true); }
QMatrix4x4 TileStream::transform(int owner) const {
    auto it=m_transforms.find(owner); return it==m_transforms.end()?QMatrix4x4():it->second;
}
void TileStream::setTransform(int owner,const QMatrix4x4& matrix) { if(owner>=0) m_transforms[owner]=matrix; }
QVector3D TileStream::center(int owner) const { auto it=m_centers.find(owner); return it==m_centers.end()?QVector3D():it->second; }
bool TileStream::visible(int owner) const { auto it=m_visibility.find(owner); return it==m_visibility.end() || it->second; }
void TileStream::setVisible(int owner,bool value) { m_visibility[owner]=value; }

// Levels follow LOD alternatives and external page boundaries, not Group depth.
void TileStream::discover(const std::shared_ptr<TileNode>& node,int level) {
    node->level=level;
    if(int(m_levels.size())<=level) m_levels.resize(size_t(level+1));
    for(size_t i=0;i<node->children.size();++i) {
        const auto& child=node->children[i];
        const int childLevel=level+(node->lod?int(i):(child->external?1:0));
        discover(child,childLevel);
        if(node->lod && (i>0 || level==0)) {
            auto& setting=m_levels[size_t(childLevel)];
            if(!setting.discovered) {
                setting.defaultMin=child->minRange; setting.defaultMax=child->maxRange;
                setting.pixelRange=node->pixelRange; setting.discovered=true;
            } else {
                setting.defaultMin=std::min(setting.defaultMin,child->minRange);
                setting.defaultMax=std::max(setting.defaultMax,child->maxRange);
            }
            if(!setting.overridden) { setting.minRange=setting.defaultMin; setting.maxRange=setting.defaultMax; }
        }
    }
}
void TileStream::setLevel(int level,bool enabled,float minimum,float maximum,bool overrideRange) {
    if(level<0 || level>=int(m_levels.size()) || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum<0 || maximum<=minimum) return;
    auto& setting=m_levels[size_t(level)]; setting.enabled=enabled; setting.overridden=overrideRange;
    setting.minRange=overrideRange?minimum:setting.defaultMin;
    setting.maxRange=overrideRange?maximum:setting.defaultMax;
}
void TileStream::resetLevels() {
    for(auto& setting:m_levels) {
        setting.enabled=true; setting.overridden=false;
        setting.minRange=setting.defaultMin; setting.maxRange=setting.defaultMax;
    }
}
bool TileStream::levelEnabled(int level) const { return m_levels[size_t(level)].enabled; }
std::pair<float,float> TileStream::range(const std::shared_ptr<TileNode>& parent,size_t index) const {
    const auto& child=parent->children[index];
    const auto& setting=m_levels[size_t(child->level)];
    if(setting.overridden && (index>0 || parent->level==0)) return {setting.minRange,setting.maxRange};
    // Changing a finer tier also moves its coarse sibling's shared boundary.
    if(index==0 && parent->children.size()==2) {
        const auto& fine=m_levels[size_t(parent->children[1]->level)];
        if(fine.overridden) return parent->pixelRange
            ? std::make_pair(child->minRange,fine.minRange)
            : std::make_pair(fine.maxRange,child->maxRange);
    }
    return {child->minRange,child->maxRange};
}

bool TileStream::sphereVisible(const QMatrix4x4& clip,const QVector3D& center,float radius) {
    for(int axis=0;axis<3;++axis) for(int sign:{-1,1}) {
        const QVector4D p=clip.row(3)+float(sign)*clip.row(axis);
        if(QVector3D::dotProduct(p.toVector3D(),center)+p.w() < -radius*p.toVector3D().length()) return false;
    }
    return true;
}

void TileStream::install(Mesh& mesh,const std::shared_ptr<TileNode>& node,int inheritedOwner) {
    if(!node) return;
    if(inheritedOwner>=0) node->owner=inheritedOwner;
    const int owner=node->owner;
    if(owner>=0 && !m_centers.count(owner)) m_centers[owner]=node->center;
    for(auto& part:node->geometry) {
        int slot;
        if(m_freeSlots.empty()) { slot=int(mesh.subMeshes.size()); mesh.subMeshes.emplace_back(); }
        else { slot=m_freeSlots.back(); m_freeSlots.pop_back(); }
        part.tileOwner=owner; part.tileLevel=node->level;
        part.savedVertices=int(part.vertices.size()); part.savedTriangles=int(part.indices.size()/3);
        part.residentBytes=qint64(part.vertices.size()*sizeof(Vertex)+part.indices.size()*sizeof(unsigned));
        // 包含解码纹理的估算；不是驱动的实际分配值。
        QImage image=part.baseImage;
        part.residentBytes+=image.sizeInBytes()*2+part.textureData.size();
        part.revision=m_revision++; part.drawable=false; part.uploaded=false;
        part.materialName=(node->name.isEmpty()?QStringLiteral("瓦片 %1").arg(owner):node->name).toStdString();
        mesh.subMeshes[size_t(slot)]=std::move(part); node->renderSlots.push_back(slot);
    }
    node->geometry.clear(); node->lastUsed=m_clock.elapsed();
    for(auto& child:node->children) install(mesh,child,inheritedOwner);
}

void TileStream::uploaded(Mesh& mesh,int slot) {
    auto& part=mesh.subMeshes[size_t(slot)]; part.uploaded=true;
    std::vector<Vertex>().swap(part.vertices); std::vector<unsigned>().swap(part.indices);
}
bool TileStream::hasEnabledGeometry(const std::shared_ptr<TileNode>& node) const {
    if(!node->renderSlots.empty() && levelEnabled(node->level)) return true;
    return std::any_of(node->children.begin(),node->children.end(),[&](const auto& child) { return hasEnabledGeometry(child); });
}
bool TileStream::ready(const std::shared_ptr<TileNode>& node,const Mesh& mesh) const {
    if(!node->loaded || (!node->renderSlots.empty() && !hasEnabledGeometry(node))) return false;
    for(int s:node->renderSlots) if(!mesh.subMeshes[size_t(s)].uploaded) return false;
    if(!node->renderSlots.empty() && levelEnabled(node->level)) return true;
    if(node->children.empty()) return node->renderSlots.empty() || levelEnabled(node->level);
    // LOD 只需要有一层可用，普通 Group 的兄弟必须全部准备好。
    if(node->lod) return std::any_of(node->children.begin(),node->children.end(),[&](const auto& c){return ready(c,mesh);});
    return std::all_of(node->children.begin(),node->children.end(),[&](const auto& c){return ready(c,mesh);});
}

void TileStream::visit(Mesh& mesh,const std::shared_ptr<TileNode>& node,
    const Camera& camera,const QSize& viewport,const QMatrix4x4& clip,bool requestOnly) {
    if(!visible(node->owner)) return;
    const auto matrix=transform(node->owner);
    const QVector3D c=matrix.map(node->center);
    const float scale=std::max({matrix.column(0).toVector3D().length(),matrix.column(1).toVector3D().length(),matrix.column(2).toVector3D().length()});
    const float radius=node->radius*scale;
    // 具有独立编辑归属的子树可能移出聚合根原来的包围体。
    if(node->owner>=0 && radius>0 && !sphereVisible(clip,c,radius)) return;
    const float distance=std::max((camera.eye()-c).length(),0.001f);
    const float pixels=camera.ortho()?radius*2*viewport.height()/std::max(camera.distance(),0.001f)
        :radius*viewport.height()/(std::tan(camera.fov()*0.0087266463f)*distance);
    node->lastUsed=m_clock.elapsed();
    node->lastVisitedFrame=m_frame;
    if(!node->loaded) {
        if(!node->loading && node->retryAfter<=m_clock.elapsed()) m_requests.push_back({node,pixels});
        return;
    }
    for(int s:node->renderSlots) if(!requestOnly && levelEnabled(node->level) && mesh.subMeshes[size_t(s)].uploaded) mesh.subMeshes[size_t(s)].drawable=true;
    if(!node->lod) { for(auto& child:node->children) visit(mesh,child,camera,viewport,clip,requestOnly); return; }
    const float metric=node->pixelRange?pixels:distance;
    bool missing=false,matched=false;
    std::set<size_t> draw;
    for(size_t i=0;i<node->children.size();++i) {
        auto& child=node->children[i];
        const auto bounds=range(node,i);
        if(metric<bounds.first || metric>=bounds.second) continue;
        matched=true;
        if(ready(child,mesh)) draw.insert(i);
        else {
            missing=true;
            visit(mesh,child,camera,viewport,clip,true);
        }
    }
    if(missing) {
        // 请求层尚未上传时保留最近的已驻留层，避免空洞。
        float best=std::numeric_limits<float>::max(); size_t fallback=node->children.size();
        for(size_t i=0;i<node->children.size();++i) if(ready(node->children[i],mesh)) {
            const auto bounds=range(node,i);
            float gap=metric<bounds.first?bounds.first-metric:std::max(0.f,metric-bounds.second);
            if(gap<best) { best=gap; fallback=i; }
        }
        if(fallback<node->children.size()) draw.insert(fallback);
    }
    if(!matched) return;
    for(size_t i:draw) visit(mesh,node->children[i],camera,viewport,clip,requestOnly);
}

void TileStream::release(Mesh& mesh,const std::shared_ptr<TileNode>& node) {
    for(auto& c:node->children) release(mesh,c);
    for(int s:node->renderSlots) {
        auto& part=mesh.subMeshes[size_t(s)];
        const auto revision=m_revision++;
        part=SubMesh(); part.resident=false; part.drawable=false; part.revision=revision;
        m_freeSlots.push_back(s);
    }
    node->renderSlots.clear();
}

void TileStream::update(Mesh& mesh,const Camera& camera,const QSize& viewport,int selected) {
    ++m_frame;
    for(auto& part:mesh.subMeshes) part.drawable=false;
    for(auto it=m_jobs.begin();it!=m_jobs.end();) {
        if(it->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready) { ++it; continue; }
        auto result=it->result.get(); auto target=it->target;
        target->loading=false;
        if(result.node) {
            auto page=result.node;
            target->geometry=std::move(page->geometry); target->children=std::move(page->children);
            target->lod=page->lod; target->pixelRange=page->pixelRange;
            target->center=page->center; target->radius=page->radius; target->loaded=true; target->failed=false;
            std::function<void(const std::shared_ptr<TileNode>&)> trace=[&](const auto& n) {
                n->ancestry=target->ancestry;
                if(target->external) n->ancestry.append(target->file);
                for(auto& c:n->children) trace(c);
            };
            for(auto& c:target->children) trace(c);
            discover(target,target->level);
            install(mesh,target,target->owner);
        } else {
            target->failed=true; target->retryAfter=m_clock.elapsed()+5000;
            ++m_stats.failures; m_stats.lastError=result.error; qWarning().noquote()<<result.error;
        }
        it=m_jobs.erase(it);
    }
    m_requests.clear();
    const float aspect=viewport.height()>0?float(viewport.width())/viewport.height():1.f;
    visit(mesh,m_root,camera,viewport,camera.projMatrix(aspect)*camera.viewMatrix());
    const int selectedOwner=selected>=0 && selected<int(mesh.subMeshes.size())?mesh.subMeshes[size_t(selected)].tileOwner:-1;
    struct Cached { std::shared_ptr<TileNode> node; qint64 used; };
    std::vector<Cached> cached;
    std::function<void(const std::shared_ptr<TileNode>&)> collect=[&](const auto& n) {
        if(n->external && n->loaded) cached.push_back({n,n->lastUsed});
        for(auto& c:n->children) collect(c);
    };
    collect(m_root);
    qint64 bytes=0; for(const auto& p:mesh.subMeshes) if(p.resident) bytes+=p.residentBytes;
    std::sort(cached.begin(),cached.end(),[](const auto& a,const auto& b){return a.used<b.used;});
    for(auto& c:cached) {
        auto n=c.node;
        if(!n->loaded || n->loading || n->owner==selectedOwner || n->lastVisitedFrame==m_frame) continue;
        // 帧内已使用的子层也会保护其外部祖先。
        bool active=false;
        std::function<void(const std::shared_ptr<TileNode>&)> check=[&](const auto& p) {
            // Request-only pages still need time for incremental GPU uploads.
            if(p->loading || p->lastVisitedFrame==m_frame) active=true;
            for(int s:p->renderSlots) if(mesh.subMeshes[size_t(s)].drawable) active=true;
            for(auto& child:p->children) check(child);
        }; check(n);
        if(active || (bytes<=budgetBytes && m_clock.elapsed()-n->lastUsed<expireMs)) continue;
        qint64 before=0; for(const auto& p:mesh.subMeshes) if(p.resident) before+=p.residentBytes;
        release(mesh,n); n->children.clear(); n->loaded=false;
        qint64 after=0; for(const auto& p:mesh.subMeshes) if(p.resident) after+=p.residentBytes;
        bytes-=before-after; ++m_stats.evicted;
    }
    std::sort(m_requests.begin(),m_requests.end(),[](const auto& a,const auto& b){return a.priority>b.priority;});
#if defined(GL3D_HAS_OSGB)
    for(auto& request:m_requests) {
        if(m_jobs.size()>=2) break;
        auto target=request.node;
        if(target->loaded || target->loading) continue;
        if(target->ancestry.contains(target->file)) {
            target->retryAfter=std::numeric_limits<qint64>::max(); target->failed=true;
            ++m_stats.failures; m_stats.lastError=QStringLiteral("分页引用环：%1").arg(target->file); continue;
        }
        // 外部引用环不会递归读入；持续深分页由预算限制。
        target->loading=true;
        const auto path=target->file;
        const auto transform=target->sourceTransform;
        const auto origin=target->origin;
        const auto cancel=m_cancel;
        const auto diffuse=target->inheritedDiffuse;
        const auto image=target->inheritedImage;
        auto task=std::make_shared<std::packaged_task<Result()>>([path,transform,origin,cancel,diffuse,image] {
            Result r;
            try { r.node=OsgbLoader::readPage(path,transform,origin,&r.error,cancel,diffuse,image); }
            catch(const std::exception& e) { r.error=QString::fromUtf8(e.what()); }
            catch(...) { r.error=QStringLiteral("OSGB 分页读取异常：%1").arg(path); }
            return r;
        });
        m_jobs.push_back({target,task->get_future()});
        QThreadPool::globalInstance()->start([task] { (*task)(); });
    }
#endif
    m_stats.resident=0; m_stats.visible=0; m_stats.bytes=0;
    for(const auto& p:mesh.subMeshes) if(p.resident) { ++m_stats.resident; m_stats.bytes+=p.residentBytes; if(p.drawable) ++m_stats.visible; }
    m_stats.loading=int(m_jobs.size()); m_stats.queued=std::max(0,int(m_requests.size())-m_stats.loading);
}

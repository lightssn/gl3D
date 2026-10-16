#include "tilestream.h"
#if defined(GL3D_HAS_OSGB)
#include "osgbloader.h"
#endif
#include <QDebug>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <set>
#include <QThreadPool>

TileStream::TileStream(Mesh& mesh) : m_root(mesh.tileRoot) {
    m_clock.start();
    discover(m_root,0);
    install(mesh, m_root, -1);
}
TileStream::~TileStream() { for(auto& job:m_jobs) job.cancel->store(true); }
bool TileStream::needsUpdate() const {
    if(m_pendingUploads || m_stats.queued>0) return true;
    return std::any_of(m_jobs.begin(),m_jobs.end(),[&](const auto& job) {
        return !job.cancel->load() && job.target->lastVisitedFrame==m_frame;
    });
}
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

bool TileStream::inView(const std::shared_ptr<TileNode>& node,const QMatrix4x4& clip) const {
    if(!visible(node->owner)) return false;
    const auto matrix=transform(node->owner);
    const float scale=std::max({matrix.column(0).toVector3D().length(),matrix.column(1).toVector3D().length(),matrix.column(2).toVector3D().length()});
    return node->owner<0 || node->radius<=0 || sphereVisible(clip,matrix.map(node->center),node->radius*scale);
}

void TileStream::install(Mesh& mesh,const std::shared_ptr<TileNode>& node,int inheritedOwner) {
    if(!node) return;
    if(node->loaded) node->indexed=true;
    if(inheritedOwner>=0) node->owner=inheritedOwner;
    const int owner=node->owner;
    if(owner>=0 && !m_centers.count(owner)) m_centers[owner]=node->center;
    for(auto& part:node->geometry) {
        int slot;
        if(m_freeSlots.empty()) { slot=int(mesh.subMeshes.size()); mesh.subMeshes.emplace_back(); }
        else { slot=m_freeSlots.back(); m_freeSlots.pop_back(); }
        part.tileOwner=owner; part.tileLevel=node->level;
        part.savedVertices=int(part.vertices.size()); part.savedTriangles=int(part.indices.size()/3);
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
    if((!node->loaded && !node->indexed) || (!node->renderSlots.empty() && !hasEnabledGeometry(node))) return false;
    for(int s:node->renderSlots) if(!mesh.subMeshes[size_t(s)].uploaded) return false;
    if(!node->renderSlots.empty() && levelEnabled(node->level)) return true;
    if(node->children.empty()) return node->loaded && (node->renderSlots.empty() || levelEnabled(node->level));
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
    for(int slot:node->renderSlots)
        if(mesh.subMeshes[size_t(slot)].resident && !mesh.subMeshes[size_t(slot)].uploaded) m_pendingUploads=true;
    if(!node->loaded) {
        // Walk the retained index first, even if this page's resources are gone.
        // A fine page can then be requested without rereading its ancestors.
        const size_t pending=m_requests.size();
        if(node->indexed) {
            const float metric=node->pixelRange?pixels:distance;
            for(size_t i=0;i<node->children.size();++i) {
                const auto bounds=range(node,i);
                if(!node->lod || (metric>=bounds.first && metric<bounds.second))
                    visit(mesh,node->children[i],camera,viewport,clip,requestOnly);
            }
        }
        bool descendantLoading=false;
        std::function<void(const std::shared_ptr<TileNode>&)> loading=[&](const auto& p) {
            descendantLoading=descendantLoading || (p->loading && p->lastVisitedFrame==m_frame);
            for(const auto& child:p->children) loading(child);
        };
        if(node->indexed) for(const auto& child:node->children) loading(child);
        // A ready finer sibling does not satisfy a newly requested coarse tier.
        // CPU-resident detail may still be uploading; do not reload ancestors then.
        std::function<bool(const std::shared_ptr<TileNode>&)> desiredResident=[&](const auto& p) {
            if(!inView(p,clip)) return true;
            if(!p->loaded && !p->indexed) return false;
            if(p->children.empty()) return p->loaded;
            if(!p->lod) return std::all_of(p->children.begin(),p->children.end(),desiredResident);
            const auto mat=transform(p->owner);
            const float d=std::max((camera.eye()-mat.map(p->center)).length(),0.001f);
            const float sc=std::max({mat.column(0).toVector3D().length(),mat.column(1).toVector3D().length(),mat.column(2).toVector3D().length()});
            const float px=camera.ortho()?p->radius*sc*2*viewport.height()/std::max(camera.distance(),0.001f)
                :p->radius*sc*viewport.height()/(std::tan(camera.fov()*0.0087266463f)*d);
            const float metric=p->pixelRange?px:d;
            for(size_t i=0;i<p->children.size();++i) {
                const auto bounds=range(p,i);
                if(metric>=bounds.first && metric<bounds.second && desiredResident(p->children[i])) return true;
            }
            return false;
        };
        if(node->external && m_requests.size()==pending && !descendantLoading && !desiredResident(node)
            && !node->loading && node->retryAfter<=m_clock.elapsed()) {
            const QVector4D projected=clip*QVector4D(c,1);
            const float w=std::max(projected.w(),0.001f);
            // Compare screen pixels so wide viewports do not favor horizontal edges.
            const float x=projected.x()/w*viewport.width(), y=projected.y()/w*viewport.height();
            m_requests.push_back({node,x*x+y*y,pixels});
        }
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
    // An external descendant owns a separate page and is reclaimed separately.
    for(auto& c:node->children) if(!c->external) release(mesh,c);
    for(int s:node->renderSlots) {
        auto& part=mesh.subMeshes[size_t(s)];
        const auto revision=m_revision++;
        part=SubMesh(); part.resident=false; part.drawable=false; part.revision=revision;
        m_freeSlots.push_back(s);
    }
    node->renderSlots.clear();
    node->loaded=false;
}

TileStats TileStream::memoryUsage(const Mesh& mesh) {
    TileStats stats;
    std::set<qint64> cpuImages,gpuImages;
    auto cpuImage=[&](const QImage& image) {
        if(!image.isNull() && cpuImages.insert(image.cacheKey()).second) stats.cpuBytes+=image.sizeInBytes();
    };
    stats.cpuBytes+=qint64(mesh.subMeshes.capacity()*sizeof(SubMesh));
    for(const auto& part:mesh.subMeshes) if(part.resident) {
        ++stats.resident; if(part.drawable) ++stats.visible;
        stats.cpuBytes+=qint64(part.vertices.capacity()*sizeof(Vertex)+part.indices.capacity()*sizeof(unsigned));
        stats.cpuBytes+=part.textureData.size()+part.metallicRoughnessData.size()+part.normalData.size();
        cpuImage(part.baseImage);
        if(part.uploaded) {
            stats.gpuBytes+=qint64(part.savedVertices)*sizeof(Vertex)+qint64(part.savedTriangles)*3*sizeof(unsigned);
            if(!part.baseImage.isNull() && gpuImages.insert(part.baseImage.cacheKey()).second) stats.gpuBytes+=part.baseImage.sizeInBytes();
        }
    }
    std::function<void(const std::shared_ptr<TileNode>&)> nodes=[&](const auto& node) {
        if(!node) return;
        stats.cpuBytes+=sizeof(TileNode)+qint64(node->children.capacity()*sizeof(std::shared_ptr<TileNode>)+node->renderSlots.capacity()*sizeof(int)+node->geometry.capacity()*sizeof(SubMesh));
        cpuImage(node->inheritedImage);
        for(const auto& child:node->children) nodes(child);
    }; nodes(mesh.tileRoot);
    stats.bytes=stats.cpuBytes+stats.gpuBytes;
    return stats;
}

void TileStream::update(Mesh& mesh,const Camera& camera,const QSize& viewport,int selected) {
    ++m_frame;
    m_pendingUploads=false;
    const float aspect=viewport.height()>0?float(viewport.width())/viewport.height():1.f;
    const auto clip=camera.projMatrix(aspect)*camera.viewMatrix();
    for(auto& part:mesh.subMeshes) part.drawable=false;
    for(auto it=m_jobs.begin();it!=m_jobs.end();) {
        // Cancel before installing a completed result from the previous view.
        if(!inView(it->target,clip)) it->cancel->store(true);
        if(it->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready) { ++it; continue; }
        auto result=it->result.get(); auto target=it->target;
        target->loading=false;
        if(it->cancel->load()) { it=m_jobs.erase(it); continue; }
        if(result.node) {
            auto page=result.node;
            // Reuse discovered external branches when restoring a parent page.
            std::function<void(const std::shared_ptr<TileNode>&,const std::shared_ptr<TileNode>&)> reuse;
            reuse=[&](const auto& oldNode,const auto& newNode) {
                for(size_t i=0;i<newNode->children.size();++i) {
                    auto& child=newNode->children[i];
                    if(child->external) {
                        auto old=std::find_if(oldNode->children.begin(),oldNode->children.end(),[&](const auto& c) {
                            return c->external && c->file==child->file;
                        });
                        if(old!=oldNode->children.end()) child=*old;
                    } else if(i<oldNode->children.size() && !oldNode->children[i]->external)
                        reuse(oldNode->children[i],child);
                }
            };
            if(target->indexed) reuse(target,page);
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
    visit(mesh,m_root,camera,viewport,clip);
    if(budgetEviction) {
        const int selectedOwner=selected>=0 && selected<int(mesh.subMeshes.size())?mesh.subMeshes[size_t(selected)].tileOwner:-1;
        struct Cached { std::shared_ptr<TileNode> node; qint64 used; };
        std::vector<Cached> cached;
        std::function<void(const std::shared_ptr<TileNode>&)> collect=[&](const auto& n) {
            if(n->external && n->loaded) cached.push_back({n,n->lastUsed});
            for(auto& c:n->children) collect(c);
        };
        collect(m_root);
        qint64 bytes=memoryUsage(mesh).bytes;
        // Keep intermediate preview tiers as long as deeper idle pages suffice.
        std::sort(cached.begin(),cached.end(),[](const auto& a,const auto& b){
            if(a.node->level!=b.node->level) return a.node->level>b.node->level;
            return a.used<b.used;
        });
        for(auto& c:cached) {
            if(bytes<=budgetBytes) break;
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
            if(active) continue;
            const qint64 before=memoryUsage(mesh).bytes;
            release(mesh,n);
            const qint64 after=memoryUsage(mesh).bytes;
            bytes-=before-after; ++m_stats.evicted;
        }
    }
    std::stable_sort(m_requests.begin(),m_requests.end(),[](const auto& a,const auto& b) {
        if(a.centerDistance!=b.centerDistance) return a.centerDistance<b.centerDistance;
        return a.pixels>b.pixels;
    });
#if defined(GL3D_HAS_OSGB)
    for(auto& request:m_requests) {
        if(m_jobs.size()>=4) break;
        auto target=request.node;
        if(target->loaded || target->loading) continue;
        if(target->ancestry.contains(target->file)) {
            target->retryAfter=std::numeric_limits<qint64>::max(); target->failed=true;
            ++m_stats.failures; m_stats.lastError=QStringLiteral("分页引用环：%1").arg(target->file); continue;
        }
        // External reference cycles are rejected before scheduling.
        target->loading=true;
        const auto path=target->file;
        const auto transform=target->sourceTransform;
        const auto origin=target->origin;
        const auto cancel=std::make_shared<std::atomic_bool>(false);
        const auto memory=std::make_shared<PageMemory>();
        const auto diffuse=target->inheritedDiffuse;
        const auto image=target->inheritedImage;
        auto task=std::make_shared<std::packaged_task<Result()>>([path,transform,origin,cancel,diffuse,image,memory] {
            Result r;
            try { r.node=OsgbLoader::readPage(path,transform,origin,&r.error,cancel,diffuse,image,memory); }
            catch(const std::exception& e) { r.error=QString::fromUtf8(e.what()); }
            catch(...) { r.error=QStringLiteral("OSGB 分页读取异常：%1").arg(path); }
            if(cancel->load()) { r.node.reset(); r.error.clear(); memory->convertedBytes=0; }
            memory->reading=false; memory->sourceBytes=0;
            return r;
        });
        m_jobs.push_back({target,task->get_future(),cancel,memory});
        QThreadPool::globalInstance()->start([task] { (*task)(); });
    }
#endif
    const auto usage=memoryUsage(mesh);
    m_stats.resident=usage.resident; m_stats.visible=usage.visible;
    m_stats.bytes=usage.bytes; m_stats.cpuBytes=usage.cpuBytes; m_stats.gpuBytes=usage.gpuBytes;
    m_stats.backgroundBytes=0; m_stats.readingJobs=0;
    for(const auto& job:m_jobs) {
        m_stats.backgroundBytes+=job.memory->sourceBytes.load()+job.memory->convertedBytes.load();
        if(job.memory->reading.load()) ++m_stats.readingJobs;
    }
    m_stats.loading=int(m_jobs.size()); m_stats.queued=int(std::count_if(m_requests.begin(),m_requests.end(),[](const auto& request) { return !request.node->loading && !request.node->failed; }));
}

int TileStream::representative(const Mesh& mesh,int slot) const {
    if(slot<0 || slot>=int(mesh.subMeshes.size())) return slot;
    const int owner=mesh.subMeshes[size_t(slot)].tileOwner;
    auto found = m_representatives.find(owner);
    return found == m_representatives.end() ? slot : found->second;
    }
void TileStream::refreshDisplay(const Mesh& mesh,bool showBounds) {
    m_representatives.clear();
    for(int i=0;i<int(mesh.subMeshes.size());++i) {
        const auto& part=mesh.subMeshes[size_t(i)];
        if(part.resident) m_representatives.emplace(part.tileOwner,i);
    }
    std::vector<QVector3D> lines;
    if(showBounds) {
        std::function<void(const std::shared_ptr<TileNode>&)> collect=[&](const auto& node) {
            QVector3D a(FLT_MAX,FLT_MAX,FLT_MAX), b(-FLT_MAX,-FLT_MAX,-FLT_MAX);
            bool found=false;
            for(int slot:node->renderSlots) {
                const auto& part=mesh.subMeshes[size_t(slot)];
                if(!part.resident || !part.drawable) continue;
                found=true;
                for(int k=0; k<3; ++k) {
                    a[k]=std::min(a[k],part.boundsMin[k]);
                    b[k]=std::max(b[k],part.boundsMax[k]);
                    }
                }
            if(found) {
                QVector3D p[8]= {{a.x(),a.y(),a.z()},{b.x(),a.y(),a.z()},
                        {b.x(),b.y(),a.z()},{a.x(),b.y(),a.z()},{a.x(),a.y(),b.z()},
                        {b.x(),a.y(),b.z()},{b.x(),b.y(),b.z()},{a.x(),b.y(),b.z()}
                    };
                const auto matrix=transform(node->owner);
                for(auto& point:p) point=matrix.map(point);
                for(int e=0; e<12; ++e) {
                    const int u=e<8?e:e-8;
                    const int v=e<4?(e+1)%4:e<8?(e-3)%4+4:e-8+4;
                    lines.push_back(p[u]);
                    lines.push_back(p[v]);
                    }
                }
            for(const auto& child:node->children) collect(child);
            };
        collect(mesh.tileRoot);
        }
    if(lines!=m_boundsLines) {
        m_boundsLines=std::move(lines);
        ++m_boundsRevision;
        }
    }

#include "meshrenderer.h"
#include "glcontext.h"
#include "mesh.h"
#include "shader.h"
#include "renderscene.h"
#include <QElapsedTimer>
#include <numeric>
#include <algorithm>
#include <QImage>
#include <cstdio>
#include <set>

MeshRenderer::~MeshRenderer() { clear(); }

qint64 MeshRenderer::textureCpuBytes() const {
    qint64 total = 0;
    std::set<qint64> images;
    for (const auto& unit : m_units) {
        for (const auto* image : {&unit.textureImage, &unit.metallicRoughnessImage, &unit.normalImage})
            if (!image->isNull() && images.insert(image->cacheKey()).second) total += image->sizeInBytes();
    }
    return total;
}

void MeshRenderer::setMipmapsEnabled(bool enabled) {
    if (m_mipmapsEnabled == enabled) return;
    m_mipmapsEnabled = enabled;
    m_drawOrderDirty = true;
    for(auto& unit:m_units) {
        releaseTexture(unit.texture); releaseTexture(unit.metallicRoughnessTexture); releaseTexture(unit.normalTexture);
    }
    for(auto& unit:m_units) {
        uploadTexture(unit.texture,unit.textureImage);
        uploadTexture(unit.metallicRoughnessTexture,unit.metallicRoughnessImage);
        uploadTexture(unit.normalTexture,unit.normalImage);
    }
}

void MeshRenderer::setSubMeshVisible(int index, bool visible) {
    if (index < 0 || index >= static_cast<int>(m_visible.size())) return;
    m_visible[static_cast<size_t>(index)] = visible;
}

bool MeshRenderer::subMeshVisible(int index) const {
    return index >= 0 && index < static_cast<int>(m_visible.size())
        ? m_visible[static_cast<size_t>(index)] && m_drawable[size_t(index)] : false;
}

void MeshRenderer::releaseTexture(unsigned int& texture) {
    if(!texture) return;
    for(auto it=m_textures.begin();it!=m_textures.end();++it) if(it->second.id==texture) {
        if(--it->second.references==0) { GLFunctions::instance().glDeleteTextures(1,&texture); m_textures.erase(it); }
        texture=0; return;
    }
    GLFunctions::instance().glDeleteTextures(1,&texture); texture=0;
}

void MeshRenderer::uploadTexture(unsigned int& texture, const QImage& image) {
    if (image.isNull()) return;
    auto found=m_textures.find(image.cacheKey());
    if(found!=m_textures.end()) { texture=found->second.id; ++found->second.references; return; }
    auto& gl = GLFunctions::instance();
    gl.glGenTextures(1, &texture);
    m_textures.emplace(image.cacheKey(),SharedTexture{texture,1});
    gl.glBindTexture(GL_TEXTURE_2D, texture);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
    if (m_mipmapsEnabled) { gl.glGenerateMipmap(GL_TEXTURE_2D); gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); }
    else gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void MeshRenderer::collectResourceIds(std::vector<unsigned int>& vaos,
                                      std::vector<unsigned int>& vbos,
                                      std::vector<unsigned int>& ebos,
                                      std::vector<unsigned int>& textures) const {
    for (size_t i = 0; i < m_units.size(); ++i) {
        const auto& u = m_units[i];
        if(!u.vao) continue;
        vaos.push_back(u.vao); vbos.push_back(u.vbo); ebos.push_back(u.ebo);
        if (u.texture) textures.push_back(u.texture);
        if (u.metallicRoughnessTexture) textures.push_back(u.metallicRoughnessTexture);
        if (u.normalTexture) textures.push_back(u.normalTexture);
    }
    for(auto* ids:{&vaos,&vbos,&ebos,&textures}) {
        std::sort(ids->begin(),ids->end()); ids->erase(std::unique(ids->begin(),ids->end()),ids->end());
    }

}

void MeshRenderer::setSubMeshTransform(int index, const QMatrix4x4& transform) {
    if (index < 0 || index >= static_cast<int>(m_transforms.size())) return;
    m_transforms[static_cast<size_t>(index)] = transform;
    }

void MeshRenderer::clear() {
    for(auto& unit:m_units) releaseUnit(unit);
    m_units.clear();
    m_drawOrder.clear();
    m_drawOrderDirty = true;
    m_drawable.clear();
    m_owners.clear();
    m_visible.clear();
    m_transforms.clear();
}

void MeshRenderer::upload(const Mesh& mesh) {
    clear();
    auto& gl = GLFunctions::instance();

    m_visible.assign(mesh.subMeshes.size(), true);
    m_transforms.assign(mesh.subMeshes.size(), QMatrix4x4());
    for (auto& transform : m_transforms) transform.setToIdentity();
    m_drawable.assign(mesh.subMeshes.size(), true);
    m_owners.assign(mesh.subMeshes.size(),-1);
    for (const auto& s : mesh.subMeshes) append(s);
    printf("[MeshRenderer] 上传 %zu 个子网格\n", m_units.size());
}

void MeshRenderer::append(const SubMesh& s, bool incremental) {
    auto& gl = GLFunctions::instance();
        DrawUnit u;
        u.indexCount = (int)s.indices.size();
        u.kd[0] = s.diffuseColor.x(); u.kd[1] = s.diffuseColor.y(); u.kd[2] = s.diffuseColor.z();
        u.metallic = s.metallicFactor; u.roughness = s.roughnessFactor;

        gl.glGenVertexArrays(1, &u.vao);
        gl.glGenBuffers(1, &u.vbo);
        gl.glGenBuffers(1, &u.ebo);
        gl.glBindVertexArray(u.vao);

        gl.glBindBuffer(GL_ARRAY_BUFFER, u.vbo);
        gl.glBufferData(GL_ARRAY_BUFFER,
                        (GLsizeiptr)(s.vertices.size() * sizeof(Vertex)),
                        incremental ? nullptr : s.vertices.data(), GL_STATIC_DRAW);
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, u.ebo);
        gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                        (GLsizeiptr)(s.indices.size() * sizeof(unsigned int)),
                        incremental ? nullptr : s.indices.data(), GL_STATIC_DRAW);

        //交错布局 pos3 normal3 uv2 与Vertex结构一致
        const int stride = sizeof(Vertex);
        gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, px));
        gl.glEnableVertexAttribArray(0);
        gl.glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, nx));
        gl.glEnableVertexAttribArray(1);
        gl.glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, u));
        gl.glEnableVertexAttribArray(2);
        gl.glBindVertexArray(0);

        //纹理上传 QImage镜像y轴匹配GL uv原点
        if (!s.baseImage.isNull() || !s.textureData.isEmpty() || !s.texturePath.isEmpty()) {
            QImage img = !s.baseImage.isNull()?s.baseImage:(!s.textureData.isEmpty() ? QImage::fromData(s.textureData) : QImage(s.texturePath));
            if (!img.isNull()) {
                QImage tex = img.convertToFormat(QImage::Format_RGBA8888);
                if (s.flipTextureVertically) tex = tex.mirrored();
                u.textureImage = tex;
                uploadTexture(u.texture, u.textureImage);
            } else {
                printf("[MeshRenderer] 纹理加载失败: %s\n",
                       s.texturePath.isEmpty() ? "GLB内嵌纹理" : qPrintable(s.texturePath));
            }
        }
        auto loadMaterialImage = [&](const QByteArray& data, const QString& path, QImage& target) {
            if (!data.isEmpty()) target = QImage::fromData(data);
            else if (!path.isEmpty()) target = QImage(path);
            if (!target.isNull()) {
                target = target.convertToFormat(QImage::Format_RGBA8888);
                if (s.flipTextureVertically) target = target.mirrored();
            }
        };
        loadMaterialImage(s.metallicRoughnessData, s.metallicRoughnessPath, u.metallicRoughnessImage);
        loadMaterialImage(s.normalData, s.normalPath, u.normalImage);
        uploadTexture(u.metallicRoughnessTexture, u.metallicRoughnessImage);
        uploadTexture(u.normalTexture, u.normalImage);
        u.revision = s.revision;
        u.compiled = !incremental;
        m_units.push_back(std::move(u));
}

void MeshRenderer::releaseUnit(DrawUnit& u) {
    m_drawOrderDirty = true;
    auto& gl=GLFunctions::instance();
    if(u.vao) gl.glDeleteVertexArrays(1,&u.vao);
    if(u.vbo) gl.glDeleteBuffers(1,&u.vbo);
    if(u.ebo) gl.glDeleteBuffers(1,&u.ebo);
    releaseTexture(u.texture);
    releaseTexture(u.metallicRoughnessTexture);
    releaseTexture(u.normalTexture);
    u=DrawUnit();
}

int MeshRenderer::visibleDrawCount() const {
    int count=0;
    for(size_t i=0;i<m_units.size();++i) if(m_units[i].indexCount && m_visible[i] && m_drawable[i]) ++count;
    return count;
}

void MeshRenderer::syncTiles(RenderScene& scene) {
    auto& mesh=scene.mesh();
    m_units.resize(mesh.subMeshes.size());
    m_visible.resize(mesh.subMeshes.size(),true);
    m_drawable.resize(mesh.subMeshes.size(),false);
    m_owners.resize(mesh.subMeshes.size(),-1);
    m_transforms.resize(mesh.subMeshes.size());
    QElapsedTimer timer; timer.start();
    int uploaded=0;
    size_t byteBudget=4*1024*1024;
    auto& gl=GLFunctions::instance();
    for(size_t i=0;i<mesh.subMeshes.size();++i) {
        auto& part=mesh.subMeshes[i];
        auto& unit=m_units[i];
        if(unit.revision!=part.revision) {
            if(!part.resident) { releaseUnit(unit); unit.revision=part.revision; }
            else if(uploaded<4 && (uploaded==0 || timer.elapsed()<4)) {
                releaseUnit(unit);
                // append 复用普通模型的材质上传路径，随后移回稳定槽位。
                append(part,true);
                m_units[i]=std::move(m_units.back()); m_units.pop_back();
                ++uploaded;
            }
        }
        auto& current=m_units[i];
        if(part.resident && current.revision==part.revision && !current.compiled && byteBudget && timer.elapsed()<4) {
            auto transfer=[&](unsigned target,unsigned buffer,const void* data,size_t size,size_t& offset) {
                const size_t bytes=std::min({size-offset,byteBudget,size_t(1024*1024)});
                if(bytes) {
                    gl.glBindBuffer(target,buffer);
                    gl.glBufferSubData(target,GLintptr(offset),GLsizeiptr(bytes),static_cast<const char*>(data)+offset);
                    offset+=bytes; byteBudget-=bytes;
                }
            };
            transfer(GL_ARRAY_BUFFER,current.vbo,part.vertices.data(),part.vertices.size()*sizeof(Vertex),current.vertexUploaded);
            // Core profile 的 EBO 更新必须绑定对应 VAO。
            gl.glBindVertexArray(current.vao);
            transfer(GL_ELEMENT_ARRAY_BUFFER,current.ebo,part.indices.data(),part.indices.size()*sizeof(unsigned),current.indexUploaded);
            gl.glBindVertexArray(0);
            if(current.vertexUploaded==part.vertices.size()*sizeof(Vertex) && current.indexUploaded==part.indices.size()*sizeof(unsigned)) {
                current.compiled=true; scene.markUploaded(int(i));
            }
        }
        current.tileTint=scene.tileTint(int(i));
        m_owners[i]=part.tileOwner;
        m_visible[i]=scene.tileVisible(int(i));
        m_drawable[i]=scene.drawable(int(i)) && m_units[i].revision==part.revision && m_units[i].compiled;
        m_transforms[i]=scene.tileTransform(int(i));
    }
}

void MeshRenderer::render(Shader& shader, int onlyIndex) {
    auto& gl = GLFunctions::instance();
    if (m_drawOrderDirty || m_drawOrder.size() != m_units.size()) {
        m_drawOrder.resize(m_units.size());
        std::iota(m_drawOrder.begin(),m_drawOrder.end(),0);
        std::stable_sort(m_drawOrder.begin(),m_drawOrder.end(),[&](size_t a,size_t b){return m_units[a].texture<m_units[b].texture;});
        m_drawOrderDirty = false;
    }
    unsigned bound[3]{UINT_MAX,UINT_MAX,UINT_MAX};
    for (size_t i : m_drawOrder) {
        if (!m_drawable[i] || !m_units[i].indexCount) continue;
        if (onlyIndex >= 0 && static_cast<int>(i) != onlyIndex &&
            (onlyIndex >= int(m_owners.size()) || m_owners[size_t(onlyIndex)]<0 || m_owners[i]!=m_owners[size_t(onlyIndex)])) continue;
        if (!m_visible[i]) continue;
        const auto& u = m_units[i];
        shader.setMat4("uModel", m_transforms[i].constData());
        shader.setInt("uUseTexture", u.texture ? 1 : 0);
        shader.setVec3("uDiffuse", u.kd[0], u.kd[1], u.kd[2]);
        shader.setVec4("uTileTint",u.tileTint.x(),u.tileTint.y(),u.tileTint.z(),u.tileTint.w());
        shader.setFloat("uMetallic", u.metallic);
        shader.setFloat("uRoughness", u.roughness);
        shader.setInt("uUseMetallicRoughness", u.metallicRoughnessTexture ? 1 : 0);
        shader.setInt("uUseNormalMap", u.normalTexture ? 1 : 0);
        if (u.texture) {
            gl.glActiveTexture(GL_TEXTURE0);
            if(bound[0]!=u.texture) { gl.glBindTexture(GL_TEXTURE_2D,u.texture); bound[0]=u.texture; }
            shader.setInt("uTexture", 0);
        }
        if (u.metallicRoughnessTexture) {
            gl.glActiveTexture(GL_TEXTURE1);
            if(bound[1]!=u.metallicRoughnessTexture) { gl.glBindTexture(GL_TEXTURE_2D,u.metallicRoughnessTexture); bound[1]=u.metallicRoughnessTexture; }
            shader.setInt("uMetallicRoughnessTexture", 1);
        }
        if (u.normalTexture) {
            gl.glActiveTexture(GL_TEXTURE2);
            if(bound[2]!=u.normalTexture) { gl.glBindTexture(GL_TEXTURE_2D,u.normalTexture); bound[2]=u.normalTexture; }
            shader.setInt("uNormalTexture", 2);
        }
        gl.glBindVertexArray(u.vao);
        gl.glDrawElements(GL_TRIANGLES, u.indexCount, GL_UNSIGNED_INT, nullptr); //draw call
    }
    gl.glBindVertexArray(0);
}

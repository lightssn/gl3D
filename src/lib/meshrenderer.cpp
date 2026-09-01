#include "meshrenderer.h"
#include "glcontext.h"
#include "shader.h"
#include <QImage>
#include <cstdio>

MeshRenderer::~MeshRenderer() { clear(); }

qint64 MeshRenderer::textureCpuBytes() const {
    qint64 total = 0;
    for (const auto& unit : m_units) {
        total += unit.textureImage.sizeInBytes();
        total += unit.metallicRoughnessImage.sizeInBytes();
        total += unit.normalImage.sizeInBytes();
    }
    return total;
}

void MeshRenderer::setMipmapsEnabled(bool enabled) {
    if (m_mipmapsEnabled == enabled) return;
    m_mipmapsEnabled = enabled;
    auto& gl = GLFunctions::instance();
    for (auto& unit : m_units) {
        if (unit.texture) gl.glDeleteTextures(1, &unit.texture);
        if (unit.metallicRoughnessTexture) gl.glDeleteTextures(1, &unit.metallicRoughnessTexture);
        if (unit.normalTexture) gl.glDeleteTextures(1, &unit.normalTexture);
        unit.texture = unit.metallicRoughnessTexture = unit.normalTexture = 0;
        uploadTexture(unit.texture, unit.textureImage);
        uploadTexture(unit.metallicRoughnessTexture, unit.metallicRoughnessImage);
        uploadTexture(unit.normalTexture, unit.normalImage);
    }
}

void MeshRenderer::setSubMeshVisible(int index, bool visible) {
    if (index < 0 || index >= static_cast<int>(m_visible.size())) return;
    m_visible[static_cast<size_t>(index)] = visible;
}

bool MeshRenderer::subMeshVisible(int index) const {
    return index >= 0 && index < static_cast<int>(m_visible.size())
        ? m_visible[static_cast<size_t>(index)] : false;
}

void MeshRenderer::uploadTexture(unsigned int& texture, const QImage& image) {
    if (image.isNull()) return;
    auto& gl = GLFunctions::instance();
    gl.glGenTextures(1, &texture);
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
        if (!m_visible[i]) continue;
        const auto& u = m_units[i];
        vaos.push_back(u.vao); vbos.push_back(u.vbo); ebos.push_back(u.ebo);
        if (u.texture) textures.push_back(u.texture);
        if (u.metallicRoughnessTexture) textures.push_back(u.metallicRoughnessTexture);
        if (u.normalTexture) textures.push_back(u.normalTexture);
    }

}

void MeshRenderer::setSubMeshTransform(int index, const QMatrix4x4& transform) {
    if (index < 0 || index >= static_cast<int>(m_transforms.size())) return;
    m_transforms[static_cast<size_t>(index)] = transform;
    }

void MeshRenderer::clear() {
    auto& gl = GLFunctions::instance();
    for (auto& u : m_units) {
        if (u.vao) gl.glDeleteVertexArrays(1, &u.vao);
        if (u.vbo) gl.glDeleteBuffers(1, &u.vbo);
        if (u.ebo) gl.glDeleteBuffers(1, &u.ebo);
        if (u.texture) gl.glDeleteTextures(1, &u.texture);
        if (u.metallicRoughnessTexture) gl.glDeleteTextures(1, &u.metallicRoughnessTexture);
        if (u.normalTexture) gl.glDeleteTextures(1, &u.normalTexture);
    }
    m_units.clear();
    m_visible.clear();
    m_transforms.clear();
}

void MeshRenderer::upload(const Mesh& mesh) {
    clear();
    auto& gl = GLFunctions::instance();

    m_visible.assign(mesh.subMeshes.size(), true);
    m_transforms.assign(mesh.subMeshes.size(), QMatrix4x4());
    for (auto& transform : m_transforms) transform.setToIdentity();
    for (const auto& s : mesh.subMeshes) {
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
                        s.vertices.data(), GL_STATIC_DRAW);
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, u.ebo);
        gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                        (GLsizeiptr)(s.indices.size() * sizeof(unsigned int)),
                        s.indices.data(), GL_STATIC_DRAW);

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
        if (!s.textureData.isEmpty() || !s.texturePath.isEmpty()) {
            QImage img = !s.textureData.isEmpty() ? QImage::fromData(s.textureData) : QImage(s.texturePath);
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
        m_units.push_back(u);
    }
    printf("[MeshRenderer] 上传 %zu 个子网格\n", m_units.size());
}

void MeshRenderer::render(Shader& shader, int onlyIndex) {
    auto& gl = GLFunctions::instance();
    for (size_t i = 0; i < m_units.size(); ++i) {
        if (onlyIndex >= 0 && static_cast<int>(i) != onlyIndex) continue;
        if (!m_visible[i]) continue;
        const auto& u = m_units[i];
        shader.setMat4("uModel", m_transforms[i].constData());
        shader.setInt("uUseTexture", u.texture ? 1 : 0);
        shader.setVec3("uDiffuse", u.kd[0], u.kd[1], u.kd[2]);
        shader.setFloat("uMetallic", u.metallic);
        shader.setFloat("uRoughness", u.roughness);
        shader.setInt("uUseMetallicRoughness", u.metallicRoughnessTexture ? 1 : 0);
        shader.setInt("uUseNormalMap", u.normalTexture ? 1 : 0);
        if (u.texture) {
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glBindTexture(GL_TEXTURE_2D, u.texture);
            shader.setInt("uTexture", 0);
        }
        if (u.metallicRoughnessTexture) {
            gl.glActiveTexture(GL_TEXTURE1);
            gl.glBindTexture(GL_TEXTURE_2D, u.metallicRoughnessTexture);
            shader.setInt("uMetallicRoughnessTexture", 1);
        }
        if (u.normalTexture) {
            gl.glActiveTexture(GL_TEXTURE2);
            gl.glBindTexture(GL_TEXTURE_2D, u.normalTexture);
            shader.setInt("uNormalTexture", 2);
        }
        gl.glBindVertexArray(u.vao);
        gl.glDrawElements(GL_TRIANGLES, u.indexCount, GL_UNSIGNED_INT, nullptr); //draw call
    }
    gl.glBindVertexArray(0);
}

#include "meshrenderer.h"
#include "glcontext.h"
#include "shader.h"
#include <QImage>
#include <cstdio>

MeshRenderer::~MeshRenderer() { clear(); }

qint64 MeshRenderer::textureCpuBytes() const {
    qint64 total = 0;
    for (const auto& unit : m_units) total += unit.textureImage.sizeInBytes();
    return total;
}

void MeshRenderer::setMipmapsEnabled(bool enabled) {
    if (m_mipmapsEnabled == enabled) return;
    m_mipmapsEnabled = enabled;
    auto& gl = GLFunctions::instance();
    for (auto& unit : m_units) {
        if (unit.texture) gl.glDeleteTextures(1, &unit.texture);
        unit.texture = 0;
        uploadTexture(unit);
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

void MeshRenderer::uploadTexture(DrawUnit& unit) {
    if (unit.textureImage.isNull()) return;
    auto& gl = GLFunctions::instance();
    gl.glGenTextures(1, &unit.texture);
    gl.glBindTexture(GL_TEXTURE_2D, unit.texture);
    const QImage& image = unit.textureImage;
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
    if (m_mipmapsEnabled) { gl.glGenerateMipmap(GL_TEXTURE_2D); gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); }
    else gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void MeshRenderer::collectResourceIds(std::vector<unsigned int>& vaos,
                                      std::vector<unsigned int>& vbos,
                                      std::vector<unsigned int>& ebos,
                                      std::vector<unsigned int>& textures) const {
    for (const auto& u : m_units) {
        vaos.push_back(u.vao); vbos.push_back(u.vbo); ebos.push_back(u.ebo);
        if (u.texture) textures.push_back(u.texture);
    }
}

void MeshRenderer::clear() {
    auto& gl = GLFunctions::instance();
    for (auto& u : m_units) {
        if (u.vao) gl.glDeleteVertexArrays(1, &u.vao);
        if (u.vbo) gl.glDeleteBuffers(1, &u.vbo);
        if (u.ebo) gl.glDeleteBuffers(1, &u.ebo);
        if (u.texture) gl.glDeleteTextures(1, &u.texture);
    }
    m_units.clear();
    m_visible.clear();
}

void MeshRenderer::upload(const Mesh& mesh) {
    clear();
    auto& gl = GLFunctions::instance();

    m_visible.assign(mesh.subMeshes.size(), true);
    for (const auto& s : mesh.subMeshes) {
        DrawUnit u;
        u.indexCount = (int)s.indices.size();
        u.kd[0] = s.diffuseColor.x(); u.kd[1] = s.diffuseColor.y(); u.kd[2] = s.diffuseColor.z();

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
                uploadTexture(u);
            } else {
                printf("[MeshRenderer] 纹理加载失败: %s\n",
                       s.texturePath.isEmpty() ? "GLB内嵌纹理" : qPrintable(s.texturePath));
            }
        }
        m_units.push_back(u);
    }
    printf("[MeshRenderer] 上传 %zu 个子网格\n", m_units.size());
}

void MeshRenderer::render(Shader& shader) {
    auto& gl = GLFunctions::instance();
    for (size_t i = 0; i < m_units.size(); ++i) {
        if (!m_visible[i]) continue;
        const auto& u = m_units[i];
        shader.setInt("uUseTexture", u.texture ? 1 : 0);
        shader.setVec3("uDiffuse", u.kd[0], u.kd[1], u.kd[2]);
        if (u.texture) {
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glBindTexture(GL_TEXTURE_2D, u.texture);
            shader.setInt("uTexture", 0);
        }
        gl.glBindVertexArray(u.vao);
        gl.glDrawElements(GL_TRIANGLES, u.indexCount, GL_UNSIGNED_INT, nullptr); //draw call
    }
    gl.glBindVertexArray(0);
}

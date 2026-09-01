#pragma once

#include "gl3d_export.h"
#include "mesh.h"
#include <vector>
#include <QImage>
#include <QMatrix4x4>

class Shader;

//网格GPU资源 每个子网格一组VAO/VBO/EBO+纹理 生命周期须在context内
class GL3D_EXPORT MeshRenderer {
public:
    MeshRenderer() = default;
    ~MeshRenderer();

    MeshRenderer(const MeshRenderer&) = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;

    void upload(const Mesh& mesh);   //CPU网格→显存 旧资源先释放
    void setMipmapsEnabled(bool enabled);
    void setSubMeshVisible(int index, bool visible);
    bool subMeshVisible(int index) const;
    void setSubMeshTransform(int index, const QMatrix4x4& transform);
    void render(Shader& shader, int onlyIndex = -1);     //逐子网格绘制
    void clear();                    //释放全部GL资源
    bool empty() const { return m_units.empty(); }
    int drawUnitCount() const { return static_cast<int>(m_units.size()); }
    qint64 textureCpuBytes() const;
    void collectResourceIds(std::vector<unsigned int>& vaos,
                            std::vector<unsigned int>& vbos,
                            std::vector<unsigned int>& ebos,
                            std::vector<unsigned int>& textures) const;

private:
    struct DrawUnit {
        unsigned int vao = 0, vbo = 0, ebo = 0;
        int indexCount = 0;
        unsigned int texture = 0, metallicRoughnessTexture = 0, normalTexture = 0;
        QImage textureImage, metallicRoughnessImage, normalImage;
        float kd[3] = {0.7f, 0.7f, 0.7f};
        float metallic = 0.0f, roughness = 1.0f;
    };
    std::vector<DrawUnit> m_units;
    std::vector<bool> m_visible;
    std::vector<QMatrix4x4> m_transforms;
    bool m_mipmapsEnabled = false;
    void uploadTexture(unsigned int& texture, const QImage& image);
};

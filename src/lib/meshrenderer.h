#pragma once

#include "gl3d_export.h"
#include "mesh.h"
#include <vector>
#include <QImage>
#include <QMatrix4x4>
#include <map>

class Shader;
class RenderScene;

//网格GPU资源 每个子网格一组VAO/VBO/EBO+纹理 生命周期须在context内
class GL3D_EXPORT MeshRenderer {
public:
    MeshRenderer() = default;
    ~MeshRenderer();

    MeshRenderer(const MeshRenderer&) = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;

    void upload(const Mesh& mesh);   //CPU网格→显存 旧资源先释放
    void syncTiles(RenderScene& scene);
    void setMipmapsEnabled(bool enabled);
    void setSubMeshVisible(int index, bool visible);
    bool subMeshVisible(int index) const;
    void setSubMeshTransform(int index, const QMatrix4x4& transform);
    void render(Shader& shader, int onlyIndex = -1);     //逐子网格绘制
    void clear();                    //释放全部GL资源
    bool empty() const { return m_units.empty(); }
    int drawUnitCount() const { return static_cast<int>(m_units.size()); }
    int visibleDrawCount() const;
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
        uint64_t revision = 0;
        size_t vertexUploaded = 0, indexUploaded = 0;
        bool compiled = true;
    };
    std::vector<DrawUnit> m_units;
    std::vector<size_t> m_drawOrder;
    bool m_drawOrderDirty = true;
    std::vector<bool> m_visible;
    std::vector<QMatrix4x4> m_transforms;
    bool m_mipmapsEnabled = false;
    std::vector<bool> m_drawable;
    std::vector<int> m_owners;
    struct SharedTexture { unsigned int id; int references; };
    std::map<qint64, SharedTexture> m_textures;
    void releaseTexture(unsigned int& texture);
    void append(const SubMesh& mesh, bool incremental = false);
    void releaseUnit(DrawUnit& unit);
    void uploadTexture(unsigned int& texture, const QImage& image);
};

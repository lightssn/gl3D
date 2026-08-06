#pragma once

#include "gl3d_export.h"
#include "mesh.h"
#include <vector>

class Shader;

//网格GPU资源 每个子网格一组VAO/VBO/EBO+纹理 生命周期须在context内
class GL3D_EXPORT MeshRenderer {
public:
    MeshRenderer() = default;
    ~MeshRenderer();

    MeshRenderer(const MeshRenderer&) = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;

    void upload(const Mesh& mesh);   //CPU网格→显存 旧资源先释放
    void render(Shader& shader);     //逐子网格绘制
    void clear();                    //释放全部GL资源
    bool empty() const { return m_units.empty(); }

private:
    struct DrawUnit {
        unsigned int vao = 0, vbo = 0, ebo = 0;
        int indexCount = 0;
        unsigned int texture = 0;    //0表示无纹理 用漫反射色
        float kd[3] = {0.7f, 0.7f, 0.7f};
    };
    std::vector<DrawUnit> m_units;
};

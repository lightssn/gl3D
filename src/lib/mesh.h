#pragma once

#include "gl3d_export.h"
#include <QMetaType>
#include <QVector3D>
#include <QString>
#include <vector>
#include <string>

//交错顶点 位置3+法线3+uv2 与着色器location 0/1/2对应
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

//按材质拆分的子网格 每个子网格独立纹理与VAO
struct SubMesh {
    std::string materialName;        //usemtl名 stl为空
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    QString texturePath;             //map_Kd解析到的磁盘路径 空则无纹理
    QVector3D diffuseColor{0.7f, 0.7f, 0.7f}; //mtl的Kd 无纹理时兜底着色
};

//整体网格 包围盒用于相机自动取景
struct GL3D_EXPORT Mesh {
    std::vector<SubMesh> subMeshes;
    QVector3D bboxMin;
    QVector3D bboxMax;

    //三角形总数 状态栏统计用
    int triangleCount() const;
    int vertexCount() const;
    QVector3D center() const { return (bboxMin + bboxMax) * 0.5f; }
    float radius() const { return (bboxMax - bboxMin).length() * 0.5f; }
};

Q_DECLARE_METATYPE(Mesh) //跨线程信号传Mesh 后台解析→主线程上传

#pragma once

#include "gl3d_export.h"
#include <QMetaType>
#include <QByteArray>
#include <QVector3D>
#include <QImage>
#include <QString>
#include <vector>
#include <string>
#include <memory>

struct TileNode;

//顶点，位置/法线/本网格纹理uv，对应着色器location 0/1/2
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    };

//子网格，存纹理与VAO，独立渲染
struct SubMesh {
    QVector3D boundsMin, boundsMax;//包围盒角点
    //瓦片
    int tileOwner = -1;//瓦片id
    int tileLevel = -1;//瓦片层级
    uint64_t revision = 0;//gpu资源号
    //统计
    int savedVertices = 0, savedTriangles = 0;//顶点 三角形数，用于上传gpu清空cpu容器后估算占用
    //状态
    //是否已上传内存，是否几何完整可绘制，是否已上传gpu
    bool resident = true, drawable = true, uploaded = false;
    //点面
    std::vector<Vertex> vertices;//顶点 3倍数
    std::vector<unsigned int> indices;//面顶点索引 三角面数x3
    //材质
    std::string materialName; //材质名 stl为空
    QImage baseImage;//基础色纹理
    QString texturePath; //外部纹理路径
    QByteArray textureData; //glb内嵌基础色纹理
    QString metallicRoughnessPath;//pbr金属粗糙度纹理路径
    QByteArray metallicRoughnessData;//pbr金属粗糙度纹理
    QString normalPath;//法线贴图路径
    QByteArray normalData;//法线贴图
    bool flipTextureVertically = true; //是否垂直翻转纹理 obj需翻转
    float metallicFactor = 0.0f;//金属度 1金属
    float roughnessFactor = 1.0f;//粗糙度 1粗糙
    QVector3D diffuseColor{0.7f, 0.7f, 0.7f}; //漫反射色 无纹理时显示
    };

//整体网格 包围盒用于相机自动取景
struct GL3D_EXPORT Mesh {
    std::shared_ptr<TileNode> tileRoot;//瓦片树根节点
    std::vector<SubMesh> subMeshes;//子网格
    QVector3D bboxMin, bboxMax;//包围盒角点
    int retainedVertexCount = 0, retainedTriangleCount = 0;//当前内存中顶点/三角形数

    // 状态栏统计用
    int triangleCount() const;//三角形总数
    int vertexCount() const;//顶点总数
    void releaseGeometry();//释放几何数据
    QVector3D center() const { //包围盒中心点
        return (bboxMin + bboxMax) * 0.5f;
        }
    float radius() const { //包围盒球半径(对角线长一半)
        return (bboxMax - bboxMin).length() * 0.5f;
        }
    };

Q_DECLARE_METATYPE(std::shared_ptr<Mesh>)//qt信号槽跨线程传递自定义变量，需在声明变量后声明元类型

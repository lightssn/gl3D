#pragma once

#include "gl3d_export.h"
#include "mesh.h"
#include <QString>

//模型加载入口 按扩展名分发obj/stl 返回是否成功
class GL3D_EXPORT MeshLoader {
public:
    static bool load(const QString& path, Mesh& outMesh, QString* err = nullptr);

private:
    static bool loadObj(const QString& path, Mesh& mesh, QString* err);
    static bool loadStl(const QString& path, Mesh& mesh, QString* err);
};

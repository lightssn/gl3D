#pragma once

#include "gl3d_export.h"
#include "mesh.h"
#include <QString>
#include <functional>

//模型加载入口 按扩展名分发obj/stl 返回是否成功
//progress回调0~100进度百分比 可空 加载在调用线程执行(阻塞)
class GL3D_EXPORT MeshLoader {
public:
    static bool load(const QString& path, Mesh& outMesh, QString* err = nullptr,
                     const std::function<void(int)>& progress = nullptr);

private:
    static bool loadObj(const QString& path, Mesh& mesh, QString* err,
                        const std::function<void(int)>& progress);
    static bool loadStl(const QString& path, Mesh& mesh, QString* err,
                        const std::function<void(int)>& progress);
};

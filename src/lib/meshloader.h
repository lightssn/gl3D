#pragma once

#include "gl3d_export.h"
#include "mesh.h"
#include <QString>
#include <QStringList>
#include <QObject>
#include <functional>

//静态接口直接解析；实例移至QThread后通过队列调用load，发出进度和结果。
class GL3D_EXPORT MeshLoader : public QObject {
    Q_OBJECT
public:
    static bool load(const QString& path, Mesh& outMesh, QString* err = nullptr, const std::function<void(int)>& progress = nullptr);

public slots:
    void load(const QStringList& paths);

signals:
    void progress(int percent);
    void finished(bool ok, const QString& error, std::shared_ptr<Mesh> mesh);

private:
    static bool loadObj(const QString& path, Mesh& mesh, QString* err, const std::function<void(int)>& progress);
    static bool loadStl(const QString& path, Mesh& mesh, QString* err, const std::function<void(int)>& progress);
    static bool loadGlb(const QString& path, Mesh& mesh, QString* err, const std::function<void(int)>& progress);
};

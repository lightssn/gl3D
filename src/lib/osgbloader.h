#pragma once

#include "gl3d_export.h"
#include "tilenode.h"
#include <atomic>
#include <functional>

class GL3D_EXPORT OsgbLoader {
public:
    static bool load(const QStringList& paths, Mesh& mesh, QString* error = nullptr,
        const std::function<void(int)>& progress = {});
    static std::shared_ptr<TileNode> readPage(const QString& path,
        const std::array<double, 16>& transform, const std::array<double, 3>& origin, QString* error,
        std::shared_ptr<std::atomic_bool> cancel = {},
        const QVector3D& inheritedDiffuse = QVector3D(0.7f, 0.7f, 0.7f),
        const QImage& inheritedImage = {}, std::shared_ptr<PageMemory> memory = {});
};

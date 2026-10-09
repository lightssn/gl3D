#pragma once

#include "mesh.h"
#include <QMatrix4x4>
#include <memory>
#include <limits>
#include <array>

// 与 OSG/API 无关的分页树，范围挂在子节点上，矩阵用于读取外部文件。
struct TileNode {
    QString name, file;
    QStringList ancestry;
    std::array<double, 16> sourceTransform{};
    std::array<double, 3> origin{};
    // External pages share the parent base material through QImage.
    QVector3D inheritedDiffuse{0.7f, 0.7f, 0.7f};
    QImage inheritedImage;
    QVector3D center;
    float radius = 0;
    float minRange = 0, maxRange = std::numeric_limits<float>::max();
    bool lod = false, pixelRange = false, external = false;
    bool loaded = true, loading = false, failed = false;
    int owner = -1;
    std::vector<SubMesh> geometry;
    std::vector<std::shared_ptr<TileNode>> children;
    std::vector<int> renderSlots;
    qint64 lastUsed = 0, retryAfter = 0;
};

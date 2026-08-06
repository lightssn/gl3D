#include "mesh.h"

int Mesh::triangleCount() const {
    int n = 0;
    for (const auto& s : subMeshes) n += static_cast<int>(s.indices.size()) / 3;
    return n;
}

int Mesh::vertexCount() const {
    int n = 0;
    for (const auto& s : subMeshes) n += static_cast<int>(s.vertices.size());
    return n;
}

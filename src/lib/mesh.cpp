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

void Mesh::releaseGeometry() {
    for (auto& subMesh : subMeshes) {
        std::vector<Vertex>().swap(subMesh.vertices);
        std::vector<unsigned int>().swap(subMesh.indices);
    }
}

#include "meshloader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QUrl>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

namespace {

constexpr quint32 GlbMagic = 0x46546C67;
constexpr quint32 JsonChunk = 0x4E4F534A;
constexpr quint32 BinChunk = 0x004E4942;

quint32 readU32(const char* data) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data));
}

struct GlbContent {
    QJsonObject root;
    QByteArray bin;
    QString directory;
};

bool parseContainer(const QString& path, GlbContent& content, QString* err) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (err) *err = "无法打开: " + path;
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 20 || readU32(bytes.constData()) != GlbMagic || readU32(bytes.constData() + 4) != 2) {
        if (err) *err = "不是有效的GLB 2.0文件";
        return false;
    }
    const quint32 declaredLength = readU32(bytes.constData() + 8);
    if (declaredLength > static_cast<quint32>(bytes.size()) || declaredLength < 20) {
        if (err) *err = "GLB文件长度无效";
        return false;
    }

    QByteArray json;
    qsizetype offset = 12;
    while (offset + 8 <= declaredLength) {
        const quint32 length = readU32(bytes.constData() + offset);
        const quint32 type = readU32(bytes.constData() + offset + 4);
        offset += 8;
        if (length > declaredLength - offset) {
            if (err) *err = "GLB数据块越界";
            return false;
        }
        if (type == JsonChunk) json = bytes.mid(offset, length);
        else if (type == BinChunk && content.bin.isEmpty()) content.bin = bytes.mid(offset, length);
        offset += length;
    }
    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &jsonError);
    if (!document.isObject()) {
        if (err) *err = "GLB JSON解析失败: " + jsonError.errorString();
        return false;
    }
    content.root = document.object();
    content.directory = QFileInfo(path).absolutePath();
    return true;
}

int componentCount(const QString& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    return 0;
}

int componentSize(int type) {
    switch (type) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: return 0;
    }
}

struct AccessorView {
    const char* data = nullptr;
    int count = 0;
    int componentType = 0;
    int components = 0;
    int stride = 0;
    bool normalized = false;
};

bool makeAccessor(const GlbContent& content, int index, AccessorView& view, QString* err) {
    const QJsonArray accessors = content.root.value("accessors").toArray();
    const QJsonArray bufferViews = content.root.value("bufferViews").toArray();
    if (index < 0 || index >= accessors.size()) {
        if (err) *err = "GLB accessor索引无效";
        return false;
    }
    const QJsonObject accessor = accessors[index].toObject();
    if (accessor.contains("sparse")) {
        if (err) *err = "暂不支持GLB sparse accessor";
        return false;
    }
    const int viewIndex = accessor.value("bufferView").toInt(-1);
    if (viewIndex < 0 || viewIndex >= bufferViews.size()) {
        if (err) *err = "GLB bufferView索引无效";
        return false;
    }
    const QJsonObject bufferView = bufferViews[viewIndex].toObject();
    if (bufferView.value("buffer").toInt(0) != 0) {
        if (err) *err = "GLB引用了非内嵌buffer";
        return false;
    }
    view.count = accessor.value("count").toInt();
    view.componentType = accessor.value("componentType").toInt();
    view.components = componentCount(accessor.value("type").toString());
    view.normalized = accessor.value("normalized").toBool(false);
    const int packedSize = componentSize(view.componentType) * view.components;
    view.stride = bufferView.value("byteStride").toInt(packedSize);
    const qsizetype viewOffset = static_cast<qsizetype>(bufferView.value("byteOffset").toDouble());
    const qsizetype accessorOffset = static_cast<qsizetype>(accessor.value("byteOffset").toDouble());
    const qsizetype viewLength = static_cast<qsizetype>(bufferView.value("byteLength").toDouble());
    const qsizetype required = view.count > 0 ? static_cast<qsizetype>(view.count - 1) * view.stride + packedSize : 0;
    if (view.count < 0 || packedSize <= 0 || view.stride < packedSize || accessorOffset < 0 ||
        required > viewLength - accessorOffset || viewOffset + accessorOffset + required > content.bin.size()) {
        if (err) *err = "GLB accessor数据越界";
        return false;
    }
    view.data = content.bin.constData() + viewOffset + accessorOffset;
    return true;
}

template<typename T>
T readScalar(const char* data) {
    T value;
    std::memcpy(&value, data, sizeof(T));
    return value;
}

float readFloatComponent(const char* data, int type, bool normalized) {
    switch (type) {
    case 5120: { const auto v = readScalar<qint8>(data); return normalized ? std::max(v / 127.0f, -1.0f) : v; }
    case 5121: { const auto v = readScalar<quint8>(data); return normalized ? v / 255.0f : v; }
    case 5122: { const auto v = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(data)); return normalized ? std::max(v / 32767.0f, -1.0f) : v; }
    case 5123: { const auto v = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data)); return normalized ? v / 65535.0f : v; }
    case 5125: return static_cast<float>(qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data)));
    case 5126: return readScalar<float>(data);
    default: return 0.0f;
    }
}

float accessorFloat(const AccessorView& view, int element, int component) {
    const char* data = view.data + static_cast<qsizetype>(element) * view.stride + component * componentSize(view.componentType);
    return readFloatComponent(data, view.componentType, view.normalized);
}

quint32 accessorIndex(const AccessorView& view, int element) {
    const char* data = view.data + static_cast<qsizetype>(element) * view.stride;
    switch (view.componentType) {
    case 5121: return readScalar<quint8>(data);
    case 5123: return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data));
    case 5125: return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data));
    default: return std::numeric_limits<quint32>::max();
    }
}

QMatrix4x4 nodeTransform(const QJsonObject& node) {
    QMatrix4x4 matrix;
    const QJsonArray values = node.value("matrix").toArray();
    if (values.size() == 16) {
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                matrix(row, column) = static_cast<float>(values[column * 4 + row].toDouble());
        return matrix;
    }
    const QJsonArray translation = node.value("translation").toArray();
    const QJsonArray rotation = node.value("rotation").toArray();
    const QJsonArray scale = node.value("scale").toArray();
    if (translation.size() == 3)
        matrix.translate(translation[0].toDouble(), translation[1].toDouble(), translation[2].toDouble());
    if (rotation.size() == 4)
        matrix.rotate(QQuaternion(rotation[3].toDouble(), rotation[0].toDouble(), rotation[1].toDouble(), rotation[2].toDouble()));
    if (scale.size() == 3)
        matrix.scale(scale[0].toDouble(), scale[1].toDouble(), scale[2].toDouble());
    return matrix;
}

QByteArray imageData(const GlbContent& content, int textureIndex, QString& externalPath) {
    const QJsonArray textures = content.root.value("textures").toArray();
    const QJsonArray images = content.root.value("images").toArray();
    const QJsonArray views = content.root.value("bufferViews").toArray();
    if (textureIndex < 0 || textureIndex >= textures.size()) return {};
    const int imageIndex = textures[textureIndex].toObject().value("source").toInt(-1);
    if (imageIndex < 0 || imageIndex >= images.size()) return {};
    const QJsonObject image = images[imageIndex].toObject();
    const int viewIndex = image.value("bufferView").toInt(-1);
    if (viewIndex >= 0 && viewIndex < views.size()) {
        const QJsonObject view = views[viewIndex].toObject();
        const qsizetype offset = static_cast<qsizetype>(view.value("byteOffset").toDouble());
        const qsizetype length = static_cast<qsizetype>(view.value("byteLength").toDouble());
        if (offset >= 0 && length >= 0 && offset + length <= content.bin.size()) return content.bin.mid(offset, length);
    }
    const QString uri = image.value("uri").toString();
    if (uri.startsWith("data:")) {
        const qsizetype comma = uri.indexOf(',');
        if (comma > 0) return QByteArray::fromBase64(uri.mid(comma + 1).toLatin1());
    } else if (!uri.isEmpty()) {
        externalPath = QDir(content.directory).filePath(QUrl::fromPercentEncoding(uri.toUtf8()));
    }
    return {};
}

void generateNormals(SubMesh& subMesh) {
    for (size_t i = 0; i + 2 < subMesh.indices.size(); i += 3) {
        Vertex& a = subMesh.vertices[subMesh.indices[i]];
        Vertex& b = subMesh.vertices[subMesh.indices[i + 1]];
        Vertex& c = subMesh.vertices[subMesh.indices[i + 2]];
        const QVector3D n = QVector3D::crossProduct(QVector3D(b.px - a.px, b.py - a.py, b.pz - a.pz),
                                                    QVector3D(c.px - a.px, c.py - a.py, c.pz - a.pz));
        for (Vertex* vertex : {&a, &b, &c}) {
            vertex->nx += n.x(); vertex->ny += n.y(); vertex->nz += n.z();
        }
    }
    for (Vertex& vertex : subMesh.vertices) {
        QVector3D normal(vertex.nx, vertex.ny, vertex.nz);
        if (normal.isNull()) normal = QVector3D(0, 1, 0);
        normal.normalize();
        vertex.nx = normal.x(); vertex.ny = normal.y(); vertex.nz = normal.z();
    }
}

} // namespace

bool MeshLoader::loadGlb(const QString& path, Mesh& mesh, QString* err,
                         const std::function<void(int)>& progress) {
    GlbContent content;
    if (!parseContainer(path, content, err)) return false;
    const QJsonArray meshes = content.root.value("meshes").toArray();
    const QJsonArray nodes = content.root.value("nodes").toArray();
    const QJsonArray materials = content.root.value("materials").toArray();
    if (meshes.isEmpty()) {
        if (err) *err = "GLB不包含网格";
        return false;
    }

    int completedPrimitives = 0;
    int totalPrimitives = 0;
    for (const QJsonValue& value : meshes) totalPrimitives += value.toObject().value("primitives").toArray().size();

    auto appendMesh = [&](int meshIndex, const QMatrix4x4& transform) -> bool {
        if (meshIndex < 0 || meshIndex >= meshes.size()) return true;
        const QJsonArray primitives = meshes[meshIndex].toObject().value("primitives").toArray();
        for (const QJsonValue& value : primitives) {
            const QJsonObject primitive = value.toObject();
            if (primitive.value("mode").toInt(4) != 4) {
                if (err) *err = "GLB仅支持三角形primitive";
                return false;
            }
            const QJsonObject attributes = primitive.value("attributes").toObject();
            AccessorView positions, normals, texcoords, indices;
            if (!makeAccessor(content, attributes.value("POSITION").toInt(-1), positions, err) || positions.components != 3) return false;
            const bool hasNormals = attributes.contains("NORMAL");
            const bool hasTexcoords = attributes.contains("TEXCOORD_0");
            const bool hasIndices = primitive.contains("indices");
            if (hasNormals && (!makeAccessor(content, attributes.value("NORMAL").toInt(), normals, err) || normals.components != 3 || normals.count != positions.count)) return false;
            if (hasTexcoords && (!makeAccessor(content, attributes.value("TEXCOORD_0").toInt(), texcoords, err) || texcoords.components != 2 || texcoords.count != positions.count)) return false;
            if (hasIndices && (!makeAccessor(content, primitive.value("indices").toInt(), indices, err) || indices.components != 1)) return false;

            SubMesh subMesh;
            subMesh.flipTextureVertically = false;
            const int materialIndex = primitive.value("material").toInt(-1);
            if (materialIndex >= 0 && materialIndex < materials.size()) {
                const QJsonObject material = materials[materialIndex].toObject();
                subMesh.materialName = material.value("name").toString().toStdString();
                const QJsonObject pbr = material.value("pbrMetallicRoughness").toObject();
                const QJsonArray factor = pbr.value("baseColorFactor").toArray();
                if (factor.size() >= 3) subMesh.diffuseColor = QVector3D(factor[0].toDouble(), factor[1].toDouble(), factor[2].toDouble());
                const int textureIndex = pbr.value("baseColorTexture").toObject().value("index").toInt(-1);
                subMesh.textureData = imageData(content, textureIndex, subMesh.texturePath);
            }

            subMesh.vertices.resize(positions.count);
            const QMatrix3x3 normalMatrix = transform.normalMatrix();
            for (int i = 0; i < positions.count; ++i) {
                Vertex& vertex = subMesh.vertices[i];
                const QVector3D position = transform.map(QVector3D(accessorFloat(positions, i, 0), accessorFloat(positions, i, 1), accessorFloat(positions, i, 2)));
                vertex.px = position.x(); vertex.py = position.y(); vertex.pz = position.z();
                if (hasNormals) {
                    const QVector3D source(accessorFloat(normals, i, 0), accessorFloat(normals, i, 1), accessorFloat(normals, i, 2));
                    QVector3D normal(normalMatrix(0, 0) * source.x() + normalMatrix(0, 1) * source.y() + normalMatrix(0, 2) * source.z(),
                                     normalMatrix(1, 0) * source.x() + normalMatrix(1, 1) * source.y() + normalMatrix(1, 2) * source.z(),
                                     normalMatrix(2, 0) * source.x() + normalMatrix(2, 1) * source.y() + normalMatrix(2, 2) * source.z());
                    normal.normalize();
                    vertex.nx = normal.x(); vertex.ny = normal.y(); vertex.nz = normal.z();
                }
                if (hasTexcoords) { vertex.u = accessorFloat(texcoords, i, 0); vertex.v = accessorFloat(texcoords, i, 1); }
            }

            const int indexCount = hasIndices ? indices.count : positions.count;
            if (indexCount % 3 != 0) {
                if (err) *err = "GLB三角形索引数量无效";
                return false;
            }
            subMesh.indices.reserve(indexCount);
            for (int i = 0; i < indexCount; ++i) {
                const quint32 index = hasIndices ? accessorIndex(indices, i) : static_cast<quint32>(i);
                if (index >= static_cast<quint32>(positions.count)) {
                    if (err) *err = "GLB顶点索引越界";
                    return false;
                }
                subMesh.indices.push_back(index);
            }
            if (!hasNormals) generateNormals(subMesh);
            mesh.subMeshes.push_back(std::move(subMesh));
            if (progress && totalPrimitives > 0) progress(++completedPrimitives * 100 / totalPrimitives);
        }
        return true;
    };

    std::function<bool(int, const QMatrix4x4&)> visitNode;
    visitNode = [&](int nodeIndex, const QMatrix4x4& parent) {
        if (nodeIndex < 0 || nodeIndex >= nodes.size()) return true;
        const QJsonObject node = nodes[nodeIndex].toObject();
        const QMatrix4x4 world = parent * nodeTransform(node);
        if (node.contains("mesh") && !appendMesh(node.value("mesh").toInt(-1), world)) return false;
        for (const QJsonValue& child : node.value("children").toArray())
            if (!visitNode(child.toInt(-1), world)) return false;
        return true;
    };

    const QJsonArray scenes = content.root.value("scenes").toArray();
    if (!scenes.isEmpty()) {
        const int sceneIndex = std::clamp(content.root.value("scene").toInt(0), 0, static_cast<int>(scenes.size()) - 1);
        for (const QJsonValue& node : scenes[sceneIndex].toObject().value("nodes").toArray())
            if (!visitNode(node.toInt(-1), QMatrix4x4())) return false;
    } else {
        for (int i = 0; i < nodes.size(); ++i)
            if (!visitNode(i, QMatrix4x4())) return false;
    }
    if (progress) progress(100);
    return !mesh.subMeshes.empty();
}

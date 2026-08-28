#include <gtest/gtest.h>
#include "meshloader.h"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <cstring>

// 临时目录写入测试文件 用例间隔离
class MeshLoaderTest : public ::testing::Test {
protected:
    QTemporaryDir dir;

    QString write(const char* name, const QByteArray& content) {
        QString p = dir.filePath(name);
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly)) return QString();
        f.write(content);
        f.close();
        return p;
    }
};

// obj基础解析 三角形+材质+uv+法线
TEST_F(MeshLoaderTest, ObjBasic) {
    write("t.mtl", "newmtl red\nKd 1.0 0.0 0.0\n");
    const char* obj =
        "mtllib t.mtl\n"
        "v 0 0 0\n" "v 1 0 0\n" "v 0 1 0\n"
        "vt 0 0\n" "vt 1 0\n" "vt 0 1\n"
        "vn 0 0 1\n"
        "usemtl red\n"
        "f 1/1/1 2/2/1 3/3/1\n";
    Mesh m;
    ASSERT_TRUE(MeshLoader::load(write("t.obj", obj), m));
    ASSERT_EQ(m.subMeshes.size(), 1);
    EXPECT_EQ(m.subMeshes[0].indices.size(), 3);
    EXPECT_EQ(m.subMeshes[0].vertices.size(), 3);
    EXPECT_FLOAT_EQ(m.subMeshes[0].diffuseColor.x(), 1.0f); // mtl的Kd生效
    EXPECT_FLOAT_EQ(m.bboxMax.x(), 1.0f);
}

// obj四边形扇形三角化 1面→2三角形 负索引解析
TEST_F(MeshLoaderTest, ObjQuadNegativeIndex) {
    const char* obj =
        "v 0 0 0\n" "v 1 0 0\n" "v 1 1 0\n" "v 0 1 0\n"
        "vn 0 0 1\n"
        "f -4//1 -3//1 -2//1 -1//1\n";
    Mesh m;
    ASSERT_TRUE(MeshLoader::load(write("q.obj", obj), m));
    EXPECT_EQ(m.triangleCount(), 2);
}

// obj缺失法线时自动生成 三角形法线应为+Z
TEST_F(MeshLoaderTest, ObjAutoNormal) {
    const char* obj =
        "v 0 0 0\n" "v 1 0 0\n" "v 0 1 0\n"
        "f 1 2 3\n";
    Mesh m;
    ASSERT_TRUE(MeshLoader::load(write("n.obj", obj), m));
    const Vertex& v = m.subMeshes[0].vertices[0];
    EXPECT_NEAR(v.nz, 1.0f, 1e-4f);
}

// 二进制stl 80头+数量+50字节/面 顶点合并平滑法线
TEST_F(MeshLoaderTest, StlBinary) {
    QByteArray d(84 + 50, '\0');
    d[80] = 1; // 1个三角形
    // 法线(0,0,1) 顶点(0,0,0)(1,0,0)(0,1,0)
    float vals[12] = {0, 0, 1,  0, 0, 0,  1, 0, 0,  0, 1, 0};
    std::memcpy(d.data() + 84, vals, sizeof(vals));
    Mesh m;
    ASSERT_TRUE(MeshLoader::load(write("b.stl", d), m));
    EXPECT_EQ(m.triangleCount(), 1);
    EXPECT_EQ(m.subMeshes[0].vertices.size(), 3);
    EXPECT_NEAR(m.subMeshes[0].vertices[0].nz, 1.0f, 1e-4f);
}

// ascii stl解析
TEST_F(MeshLoaderTest, StlAscii) {
    const char* stl =
        "solid t\n"
        "facet normal 0 0 1\nouter loop\n"
        "vertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
        "endloop\nendfacet\n"
        "endsolid t\n";
    Mesh m;
    ASSERT_TRUE(MeshLoader::load(write("a.stl", stl), m));
    EXPECT_EQ(m.triangleCount(), 1);
}

// 非法文件与不支持格式
TEST_F(MeshLoaderTest, RejectBadInput) {
    Mesh m;
    QString err;
    EXPECT_FALSE(MeshLoader::load(write("x.fbx", "junk"), m, &err));
    EXPECT_FALSE(MeshLoader::load(dir.filePath("missing.obj"), m, &err));
}

// GLB 2.0内嵌三角形，验证buffer/accessor和内嵌基础色纹理
TEST_F(MeshLoaderTest, GlbEmbeddedMesh) {
    QByteArray bin;
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const quint16 indices[] = {0, 1, 2};
    bin.append(reinterpret_cast<const char*>(positions), sizeof(positions));
    bin.append(reinterpret_cast<const char*>(indices), sizeof(indices));
    bin.append("img", 3);
    while (bin.size() % 4) bin.append('\0');

    QJsonObject root;
    root["asset"] = QJsonObject{{"version", "2.0"}};
    root["buffers"] = QJsonArray{QJsonObject{{"byteLength", bin.size()}}};
    root["bufferViews"] = QJsonArray{
        QJsonObject{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", int(sizeof(positions))}},
        QJsonObject{{"buffer", 0}, {"byteOffset", int(sizeof(positions))}, {"byteLength", int(sizeof(indices))}},
        QJsonObject{{"buffer", 0}, {"byteOffset", int(sizeof(positions) + sizeof(indices))}, {"byteLength", 3}}
    };
    root["accessors"] = QJsonArray{
        QJsonObject{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
        QJsonObject{{"bufferView", 1}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}
    };
    root["images"] = QJsonArray{QJsonObject{{"bufferView", 2}, {"mimeType", "image/png"}}};
    root["textures"] = QJsonArray{QJsonObject{{"source", 0}}};
    root["materials"] = QJsonArray{QJsonObject{{"pbrMetallicRoughness", QJsonObject{{"baseColorTexture", QJsonObject{{"index", 0}}}}}}};
    root["meshes"] = QJsonArray{QJsonObject{{"primitives", QJsonArray{QJsonObject{{"attributes", QJsonObject{{"POSITION", 0}}}, {"indices", 1}, {"material", 0}}}}}};
    root["nodes"] = QJsonArray{QJsonObject{{"mesh", 0}}};
    root["scenes"] = QJsonArray{QJsonObject{{"nodes", QJsonArray{0}}}};
    root["scene"] = 0;

    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size() % 4) json.append(' ');
    QByteArray glb;
    auto appendU32 = [&](quint32 value) {
        const quint32 little = qToLittleEndian(value);
        glb.append(reinterpret_cast<const char*>(&little), sizeof(little));
    };
    appendU32(0x46546C67); appendU32(2); appendU32(12 + 8 + json.size() + 8 + bin.size());
    appendU32(json.size()); appendU32(0x4E4F534A); glb.append(json);
    appendU32(bin.size()); appendU32(0x004E4942); glb.append(bin);

    Mesh mesh;
    QString error;
    ASSERT_TRUE(MeshLoader::load(write("triangle.glb", glb), mesh, &error)) << qPrintable(error);
    ASSERT_EQ(mesh.subMeshes.size(), 1);
    EXPECT_EQ(mesh.vertexCount(), 3);
    EXPECT_EQ(mesh.triangleCount(), 1);
    EXPECT_EQ(mesh.subMeshes[0].textureData, QByteArray("img", 3));
    EXPECT_FALSE(mesh.subMeshes[0].flipTextureVertically);
    EXPECT_NEAR(mesh.subMeshes[0].vertices[0].nz, 1.0f, 1e-4f);
}

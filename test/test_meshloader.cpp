#include <gtest/gtest.h>
#include "meshloader.h"
#include <QTemporaryDir>
#include <QFile>
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

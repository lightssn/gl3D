#include "meshloader.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QDataStream>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <cfloat>

bool MeshLoader::load(const QString& path, Mesh& outMesh, QString* err,
                      const std::function<void(int)>& progress) {
    QString suffix = QFileInfo(path).suffix().toLower();
    bool ok = false;
    if (suffix == "obj") ok = loadObj(path, outMesh, err, progress);
    else if (suffix == "stl") ok = loadStl(path, outMesh, err, progress);
    else if (err) *err = "不支持的格式: " + suffix;
    if (!ok) return false;

    //汇总包围盒
    if (outMesh.subMeshes.empty()) {
        if (err) *err = "空模型";
        return false;
        }
    QVector3D mn(FLT_MAX, FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const auto& s : outMesh.subMeshes)
        for (const auto& v : s.vertices) {
            QVector3D p(v.px, v.py, v.pz);
            mn = QVector3D(std::min(mn.x(), p.x()), std::min(mn.y(), p.y()), std::min(mn.z(), p.z()));
            mx = QVector3D(std::max(mx.x(), p.x()), std::max(mx.y(), p.y()), std::max(mx.z(), p.z()));
            }
    outMesh.bboxMin = mn;
    outMesh.bboxMax = mx;
    return true;
    }

//mtl材质参数
struct MtlInfo {
    QVector3D kd{0.7f, 0.7f, 0.7f};
    QString mapKd; //原始相对路径
    };

//解析mtl文件 返回 材质名→参数
static std::unordered_map<std::string, MtlInfo> parseMtl(const QString& mtlPath) {
    std::unordered_map<std::string, MtlInfo> result;
    QFile f(mtlPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return result;
    QTextStream in(&f);
    MtlInfo* cur = nullptr;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;
        if (parts[0] == "newmtl" && parts.size() >= 2) {
            cur = &result[parts[1].toStdString()];
            }
        else if (cur && parts[0] == "Kd" && parts.size() >= 4) {
            cur->kd = QVector3D(parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat());
            }
        else if (cur && parts[0] == "map_Kd" && parts.size() >= 2) {
            cur->mapKd = parts.last(); //取末尾 兼容含空格前缀
            }
        }
    return result;
    }

//查找纹理实际路径 依次尝试 obj目录 → tex子目录
static QString resolveTexture(const QString& objDir, const QString& relPath) {
    QString p1 = objDir + "/" + relPath;
    if (QFile::exists(p1)) return p1;
    QString p2 = objDir + "/tex/" + QFileInfo(relPath).fileName();
    if (QFile::exists(p2)) return p2;
    return QString();
    }

//快速统计文件行数 进度百分比用 文本格式才需要
static int countLines(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    return f.readAll().count('\n');
    }

bool MeshLoader::loadObj(const QString& path, Mesh& mesh, QString* err,
                         const std::function<void(int)>& progress) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = "无法打开: " + path;
        return false;
        }
    const QString objDir = QFileInfo(path).absolutePath();
    const int totalLines = countLines(path); //进度基准 预读一次

    //原始属性池 面索引引用它们
    std::vector<QVector3D> positions;
    std::vector<QVector3D> texcoords; //只用xy
    std::vector<QVector3D> normals;
    std::unordered_map<std::string, MtlInfo> materials;

    QTextStream in(&f);
    SubMesh* cur = nullptr;
    int parsedLines = 0; //已解析行数 进度用

    //obj顶点去重，(vi,ti,ni)组合→子网格内顶点下标
    std::unordered_map<long long, unsigned int> vertMap;

    auto finishVertMap = [&]() {
        vertMap.clear();
        vertMap.reserve(4096);
        };

    while (!in.atEnd()) {
        const QString line = in.readLine();
        //进度按已读行数 每512行报一次 避免回调过频
        int linesRead = ++parsedLines;
        if (progress && (linesRead & 0x1FF) == 0 && totalLines > 0)
            progress(std::min(linesRead * 100 / totalLines, 99));
        const QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
        if (parts.isEmpty() || parts[0].startsWith('#')) continue;
        const QString& tag = parts[0];

        if (tag == "v" && parts.size() >= 4) {
            positions.emplace_back(parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat());
            }
        else if (tag == "vt" && parts.size() >= 3) {
            texcoords.emplace_back(parts[1].toFloat(), parts[2].toFloat(), 0.0f);
            }
        else if (tag == "vn" && parts.size() >= 4) {
            normals.emplace_back(parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat());
            }
        else if (tag == "mtllib" && parts.size() >= 2) {
            materials = parseMtl(objDir + "/" + parts.last());
            }
        else if (tag == "usemtl" && parts.size() >= 2) {
            //查找是否已有该材质子网格 复用避免碎片化
            cur = nullptr;
            for (auto& s : mesh.subMeshes)
                if (s.materialName == parts[1].toStdString()) {
                    cur = &s;
                    break;
                    }
            if (!cur) {
                mesh.subMeshes.emplace_back();
                cur = &mesh.subMeshes.back();
                cur->materialName = parts[1].toStdString();
                auto it = materials.find(cur->materialName);
                if (it != materials.end()) {
                    cur->diffuseColor = it->second.kd;
                    cur->texturePath = resolveTexture(objDir, it->second.mapKd);
                    }
                }
            finishVertMap();
            }
        else if (tag == "f" && parts.size() >= 4) {
            if (!cur) { //无usemtl的obj 建默认子网格
                mesh.subMeshes.emplace_back();
                cur = &mesh.subMeshes.back();
                finishVertMap();
                }
            //解析每个角点 vi/ti/ni 支持负索引 扇形三角化多边形
            unsigned int faceIdx[64];
            int corner = 0;
            for (int i = 1; i < parts.size() && corner < 64; ++i) {
                const QStringList ids = parts[i].split('/');
                auto fix = [](int idx, int count) {
                    return idx < 0 ? count + idx : idx - 1;
                    };
                int vi = fix(ids.value(0).toInt(), (int)positions.size());
                int ti = ids.size() > 1 && !ids[1].isEmpty() ? fix(ids[1].toInt(), (int)texcoords.size()) : -1;
                int ni = ids.size() > 2 && !ids[2].isEmpty() ? fix(ids[2].toInt(), (int)normals.size()) : -1;

                long long key = ((long long)(vi + 1) << 40) ^ ((long long)(ti + 1) << 20) ^ (ni + 1);
                auto it = vertMap.find(key);
                if (it != vertMap.end()) {
                    faceIdx[corner++] = it->second;
                    continue;
                    }

                Vertex v{};
                if (vi >= 0 && vi < (int)positions.size()) {
                    v.px = positions[vi].x();
                    v.py = positions[vi].y();
                    v.pz = positions[vi].z();
                    }
                if (ni >= 0 && ni < (int)normals.size()) {
                    v.nx = normals[ni].x();
                    v.ny = normals[ni].y();
                    v.nz = normals[ni].z();
                    }
                if (ti >= 0 && ti < (int)texcoords.size()) {
                    v.u = texcoords[ti].x();
                    v.v = texcoords[ti].y();
                    }
                unsigned int newIdx = (unsigned int)cur->vertices.size();
                cur->vertices.push_back(v);
                vertMap.emplace(key, newIdx);
                faceIdx[corner++] = newIdx;
                }
            //扇形三角化 (0,i,i+1)
            for (int i = 1; i + 1 < corner; ++i) {
                cur->indices.push_back(faceIdx[0]);
                cur->indices.push_back(faceIdx[i]);
                cur->indices.push_back(faceIdx[i + 1]);
                }
            }
        }
    if (progress) progress(100); //解析完成 上传/取景在主线程做

    //丢弃空子网格
    for (auto it = mesh.subMeshes.begin(); it != mesh.subMeshes.end();)
        it = it->indices.empty() ? mesh.subMeshes.erase(it) : it + 1;

    //法线缺失时按面积加权计算平滑法线
    for (auto& s : mesh.subMeshes) {
        bool hasNormal = false;
        for (const auto& v : s.vertices)
            if (v.nx != 0 || v.ny != 0 || v.nz != 0) {
                hasNormal = true;
                break;
                }
        if (hasNormal) continue;
        for (size_t i = 0; i + 2 < s.indices.size(); i += 3) {
            Vertex& a = s.vertices[s.indices[i]];
            Vertex& b = s.vertices[s.indices[i + 1]];
            Vertex& c = s.vertices[s.indices[i + 2]];
            QVector3D ab(b.px - a.px, b.py - a.py, b.pz - a.pz);
            QVector3D ac(c.px - a.px, c.py - a.py, c.pz - a.pz);
            QVector3D n = QVector3D::crossProduct(ab, ac); //长度即2倍面积 自带权重
            for (Vertex* v : {
                        &a, &b, &c
                    }) {
                v->nx += n.x();
                v->ny += n.y();
                v->nz += n.z();
                }
            }
        for (auto& v : s.vertices) {
            QVector3D n(v.nx, v.ny, v.nz);
            if (n.isNull()) n = QVector3D(0, 1, 0);
            n.normalize();
            v.nx = n.x();
            v.ny = n.y();
            v.nz = n.z();
            }
        }
    return !mesh.subMeshes.empty();
    }//loadObj

//二进制stl三角形50字节 法线12+顶点36+属性2
#pragma pack(push, 1)
struct StlTri {
    float normal[3];
    float verts[9];
    unsigned short attr;
    };
#pragma pack(pop)

bool MeshLoader::loadStl(const QString& path, Mesh& mesh, QString* err, const std::function<void(int)>& progress) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = "无法打开: " + path;
        return false;
        }
    const QByteArray data = f.readAll();
    if (data.size() < 84) {
        if (err) *err = "文件过小";
        return false;
        }

    mesh.subMeshes.emplace_back();
    SubMesh& sub = mesh.subMeshes.back();

    //顶点去重字典，自定义哈希，值为自定义面索引
    std::unordered_map<long long, unsigned int> vertMap;
    //xyz float放大为整形(小数5位精度)拼接得到int64(long long)哈希值
    auto keyOf = [](float x, float y, float z) {
        long long xi = llround(x * 1e5), yi = llround(y * 1e5), zi = llround(z * 1e5);
        return (xi << 42) ^ (yi << 21) ^ zi;
        };
    //读取每个顶点位置p、面法线n，合并同位置点的面法线
    auto addVertex = [&](const float* p, const QVector3D& n) {
        long long key = keyOf(p[0], p[1], p[2]);
        auto it = vertMap.find(key); //算哈希
        unsigned int idx;
        if (it != vertMap.end()) { //有重复点
            idx = it->second;
            Vertex& v = sub.vertices[idx];
            v.nx += n.x();
            v.ny += n.y();
            v.nz += n.z(); //累加面法线为平滑顶点法线
            }
        else { //新建顶点
            Vertex v{};
            v.px = p[0];
            v.py = p[1];
            v.pz = p[2];
            v.nx = n.x();
            v.ny = n.y();
            v.nz = n.z();
            idx = (unsigned int)sub.vertices.size();
            sub.vertices.push_back(v);
            vertMap.emplace(key, idx);
            }
        sub.indices.push_back(idx);
        };
    //潜在问题：精度1e5，世界坐标可能溢出

    //判定二进制: 84+50*n恰好等于文件大小
    unsigned int triCount = 0;
    std::memcpy(&triCount, data.constData() + 80, 4);
    const bool isBinary = (84ULL + 50ULL * triCount == (unsigned long long)data.size());

    if (isBinary) {
        const StlTri* tris = reinterpret_cast<const StlTri*>(data.constData() + 84);
        sub.vertices.reserve(triCount * 2);
        sub.indices.reserve(triCount * 3);
        for (unsigned int i = 0; i < triCount; ++i) {
            //进度按三角形数 每1024个报一次
            if (progress && (i & 0x3FF) == 0)
                progress((int)(i * 100 / triCount));
            QVector3D n(tris[i].normal[0], tris[i].normal[1], tris[i].normal[2]);
            if (n.isNull()) { //法线为零则叉积计算
                QVector3D a(tris[i].verts[0], tris[i].verts[1], tris[i].verts[2]);
                QVector3D b(tris[i].verts[3], tris[i].verts[4], tris[i].verts[5]);
                QVector3D c(tris[i].verts[6], tris[i].verts[7], tris[i].verts[8]);
                n = QVector3D::crossProduct(b - a, c - a);
                }
            addVertex(tris[i].verts + 0, n);
            addVertex(tris[i].verts + 3, n);
            addVertex(tris[i].verts + 6, n);
            }
        }
    else {
        //ascii stl 逐行解析vertex
        QTextStream in(data);
        QVector3D faceN;
        const int totalLines = data.count('\n'); //进度基准
        int parsedLines = 0;
        while (!in.atEnd()) {
            const QString line = in.readLine();
            //进度按已读行数 每512行报一次
            int linesRead = ++parsedLines;
            if (progress && (linesRead & 0x1FF) == 0 && totalLines > 0)
                progress(std::min(linesRead * 100 / totalLines, 99));
            const QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 5 && parts[0] == "facet")
                faceN = QVector3D(parts[2].toFloat(), parts[3].toFloat(), parts[4].toFloat());
            else if (parts.size() >= 4 && parts[0] == "vertex") {
                float p[3] = {parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat()};
                addVertex(p, faceN);
                }
            }
        }
    if (progress) progress(100); //解析完成

    //归一化平滑法线
    for (auto& v : sub.vertices) {
        QVector3D n(v.nx, v.ny, v.nz);
        if (n.isNull()) n = QVector3D(0, 1, 0);
        n.normalize();
        v.nx = n.x();
        v.ny = n.y();
        v.nz = n.z();
        }
    return !sub.indices.empty();
    }

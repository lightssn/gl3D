#pragma once

#include "mesh.h"
#include "camera.h"
#include "gl3d_export.h"
#include <QSize>
#include <QString>
#include <QVector3D>
#include <future>
#include <array>
#include <cfloat>
#include <memory>
#include <vector>

// OSGB is decoded by osgDB only. Visibility, paging and cache ownership stay here.
class GL3D_EXPORT OsgbStream {
public:
    struct Stats { int tiles = 0, visible = 0, pending = 0, failed = 0; qint64 cacheBytes = 0; };
    OsgbStream();
    ~OsgbStream();
    OsgbStream(const OsgbStream&) = delete;
    OsgbStream& operator=(const OsgbStream&) = delete;
    bool open(const QString& file, QString* error);
    void clear();
    bool update(const Camera& camera, const QSize& viewport);
    bool available() const;
    const Mesh& visibleMesh() const { return m_visibleMesh; }
    const std::vector<QVector3D>& tileMins() const { return m_tileMins; }
    const std::vector<QVector3D>& tileMaxs() const { return m_tileMaxs; }
    const std::vector<int>& tileIds() const { return m_tileIds; }
    float rootRadius() const { return m_tiles.empty() ? 1.0f : m_tiles.front().radius; }
    QVector3D rootCenter() const { return m_tiles.empty() ? QVector3D() : m_tiles.front().center; }
    Stats stats() const;
private:
    using MatrixArray = std::array<double, 16>;
    static constexpr MatrixArray identityMatrix() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
    struct Child {
        QString path;
        QVector3D center;
        float radius = 0, rangeMin = 0, rangeMax = FLT_MAX;
        bool rangeSpecified = false, pixelRange = false;
        MatrixArray matrix = identityMatrix();
    };
    struct Payload { Mesh mesh; std::vector<Child> children; QVector3D center; float radius = 0; qint64 bytes = 0; QString error; std::array<double, 3> origin{0, 0, 0}; };
    struct Tile {
        QString path;
        QVector3D center;
        float radius = 0;
        float rangeMin = 0, rangeMax = FLT_MAX;
        bool rangeSpecified = false, pixelRange = false;
        MatrixArray matrix = identityMatrix();
        std::shared_ptr<Payload> payload;
        std::future<Payload> request;
        std::vector<int> children;
        quint64 lastUsed = 0;
        bool visible = false;
        bool failed = false;
    };
    static Payload readTile(const QString& path, const std::array<double, 3>& origin, bool root,
                            const MatrixArray& parent = identityMatrix());
    void install(int index, Payload payload);
    bool inFrustum(const QVector3D& center, float radius) const;
    void traverse(int index, const Camera& camera, const QSize& viewport,
                  std::vector<int>& visible, std::vector<int>& requests);
    void rebuild(const std::vector<int>& visible);
    void evict(const std::vector<int>& visible);
    std::vector<Tile> m_tiles;
    Mesh m_visibleMesh;
    std::vector<QVector3D> m_tileMins, m_tileMaxs;
    std::vector<int> m_tileIds;
    float m_planes[6][4] = {};
    quint64 m_frame = 0;
    qint64 m_cachedBytes = 0;
    bool m_available = false;
    std::array<double, 3> m_origin{0, 0, 0};
};

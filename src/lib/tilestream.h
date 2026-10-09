#pragma once

#include "tilenode.h"
#include "camera.h"
#include <future>
#include <QElapsedTimer>
#include <map>
#include <atomic>

struct TileStats {
    int resident = 0, visible = 0, loading = 0, queued = 0, failures = 0;
    qint64 bytes = 0, evicted = 0;
    QString lastError;
};

// 单线程修改场景，后台任务只返回独立分页数据。
class GL3D_EXPORT TileStream {
public:
    explicit TileStream(Mesh& mesh);
    ~TileStream();
    TileStream(const TileStream&) = delete;
    TileStream& operator=(const TileStream&) = delete;
    void update(Mesh& mesh, const Camera& camera, const QSize& viewport, int selected);
    void uploaded(Mesh& mesh, int slot);
    QMatrix4x4 transform(int owner) const;
    void setTransform(int owner, const QMatrix4x4& matrix);
    QVector3D center(int owner) const;
    bool visible(int owner) const;
    void setVisible(int owner, bool value);
    const TileStats& stats() const { return m_stats; }
    qint64 budgetBytes = 512ll * 1024 * 1024;
    qint64 expireMs = 10000;
    static bool sphereVisible(const QMatrix4x4& clip, const QVector3D& center, float radius);
private:
    struct Request { std::shared_ptr<TileNode> node; float priority; };
    struct Result { std::shared_ptr<TileNode> node; QString error; };
    struct Job { std::shared_ptr<TileNode> target; std::future<Result> result; };
    std::shared_ptr<TileNode> m_root;
    std::vector<Job> m_jobs;
    std::vector<Request> m_requests;
    std::vector<int> m_freeSlots;
    std::map<int, QMatrix4x4> m_transforms;
    std::map<int, QVector3D> m_centers;
    std::map<int, bool> m_visibility;
    QElapsedTimer m_clock;
    uint64_t m_revision = 1;
    TileStats m_stats;
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
    void install(Mesh& mesh, const std::shared_ptr<TileNode>& node, int inheritedOwner);
    void release(Mesh& mesh, const std::shared_ptr<TileNode>& node);
    bool ready(const std::shared_ptr<TileNode>& node, const Mesh& mesh) const;
    void visit(Mesh& mesh, const std::shared_ptr<TileNode>& node,
               const Camera& camera, const QSize& viewport, const QMatrix4x4& clip, bool requestOnly = false);
};

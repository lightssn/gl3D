#pragma once

#include "tilenode.h"
#include "camera.h"
#include <future>
#include <QElapsedTimer>
#include <map>
#include <atomic>

struct TileLevelSettings {
    bool enabled = true, overridden = false, pixelRange = false, discovered = false;
    float minRange = 0, maxRange = std::numeric_limits<float>::max();
    float defaultMin = 0, defaultMax = std::numeric_limits<float>::max();
};

struct TileStats {
    int resident = 0, visible = 0, loading = 0, queued = 0, failures = 0;
    qint64 bytes = 0, evicted = 0;
    qint64 cpuBytes = 0, gpuBytes = 0, backgroundBytes = 0;
    int readingJobs = 0;
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
    bool needsUpdate() const;
    void refreshDisplay(const Mesh& mesh,bool showBounds);
    int representative(const Mesh& mesh,int slot) const;
    const std::vector<QVector3D>& boundsLines() const { return m_boundsLines; }
    uint64_t boundsRevision() const { return m_boundsRevision; }
    const std::vector<TileLevelSettings>& levels() const { return m_levels; }
    void setLevel(int level, bool enabled, float minimum, float maximum, bool overrideRange);
    void resetLevels();
    qint64 budgetBytes = 1024ll * 1024 * 1024;
    bool budgetEviction = false;
    static TileStats memoryUsage(const Mesh& mesh);
    static bool sphereVisible(const QMatrix4x4& clip, const QVector3D& center, float radius);
private:
    std::map<int,int> m_representatives;
    std::vector<QVector3D> m_boundsLines;
    uint64_t m_boundsRevision = 0;
    bool m_pendingUploads = false;
    struct Request { std::shared_ptr<TileNode> node; float centerDistance, pixels; };
    struct Result { std::shared_ptr<TileNode> node; QString error; };
    struct Job { std::shared_ptr<TileNode> target; std::future<Result> result; std::shared_ptr<std::atomic_bool> cancel; std::shared_ptr<PageMemory> memory; };
    std::shared_ptr<TileNode> m_root;
    std::vector<Job> m_jobs;
    std::vector<Request> m_requests;
    std::vector<int> m_freeSlots;
    std::map<int, QMatrix4x4> m_transforms;
    std::map<int, QVector3D> m_centers;
    std::map<int, bool> m_visibility;
    QElapsedTimer m_clock;
    uint64_t m_revision = 1, m_frame = 0;
    TileStats m_stats;
    std::vector<TileLevelSettings> m_levels;
    void discover(const std::shared_ptr<TileNode>& node, int level);
    bool levelEnabled(int level) const;
    bool hasEnabledGeometry(const std::shared_ptr<TileNode>& node) const;
    std::pair<float,float> range(const std::shared_ptr<TileNode>& parent, size_t index) const;
    bool inView(const std::shared_ptr<TileNode>& node, const QMatrix4x4& clip) const;
    void install(Mesh& mesh, const std::shared_ptr<TileNode>& node, int inheritedOwner);
    void release(Mesh& mesh, const std::shared_ptr<TileNode>& node);
    bool ready(const std::shared_ptr<TileNode>& node, const Mesh& mesh) const;
    void visit(Mesh& mesh, const std::shared_ptr<TileNode>& node,
               const Camera& camera, const QSize& viewport, const QMatrix4x4& clip, bool requestOnly = false);
};

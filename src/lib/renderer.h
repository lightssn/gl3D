#pragma once

#include "gl3d_export.h"
#include <QMatrix4x4>
#include <QtGlobal>
#include <functional>
#include <vector>

class Scene;

// CPU draw state shared by renderers. No graphics API resources live here.
class GL3D_EXPORT Renderer {
public:
    Renderer() = default;
    virtual ~Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void setSubMeshVisible(int index, bool visible);
    bool subMeshVisible(int index) const;
    void setSubMeshTransform(int index, const QMatrix4x4& transform);

protected:
    void synchronizeSlot(const Scene& scene, size_t index, bool ready);
    void resetSlots(size_t count);
    void resizeSlots(size_t count);
    void clearSlots();
    bool acceptsDraw(size_t index, int selected = -1) const;
    void updateDrawOrder(size_t count, const std::function<qint64(size_t)>& materialKey);

    std::vector<size_t> m_drawOrder;
    bool m_drawOrderDirty = true;
    std::vector<bool> m_visible;
    std::vector<bool> m_drawable;
    std::vector<int> m_owners;
    std::vector<QMatrix4x4> m_matrices;
};

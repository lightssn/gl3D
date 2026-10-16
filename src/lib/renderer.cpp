#include "renderer.h"
#include "scene.h"
#include <algorithm>
#include <numeric>

void Renderer::resetSlots(size_t count) {
    clearSlots();
    m_visible.assign(count, true);
    m_drawable.assign(count, true);
    m_owners.assign(count, -1);
    m_matrices.assign(count, QMatrix4x4());
}

void Renderer::resizeSlots(size_t count) {
    m_visible.resize(count, true);
    m_drawable.resize(count, false);
    m_owners.resize(count, -1);
    m_matrices.resize(count);
}

void Renderer::clearSlots() {
    m_drawOrder.clear();
    m_drawOrderDirty = true;
    m_visible.clear();
    m_drawable.clear();
    m_owners.clear();
    m_matrices.clear();
}

void Renderer::setSubMeshVisible(int index, bool visible) {
    if (index >= 0 && size_t(index) < m_visible.size()) m_visible[size_t(index)] = visible;
}

bool Renderer::subMeshVisible(int index) const {
    return index >= 0 && size_t(index) < m_visible.size()
        && m_visible[size_t(index)] && m_drawable[size_t(index)];
}

void Renderer::setSubMeshTransform(int index, const QMatrix4x4& transform) {
    if (index >= 0 && size_t(index) < m_matrices.size()) m_matrices[size_t(index)] = transform;
}

bool Renderer::acceptsDraw(size_t index, int selected) const {
    if (index >= m_visible.size() || !m_visible[index] || !m_drawable[index]) return false;
    if (selected < 0 || index == size_t(selected)) return true;
    return size_t(selected) < m_owners.size() && m_owners[size_t(selected)] >= 0
        && m_owners[index] == m_owners[size_t(selected)];
}

void Renderer::updateDrawOrder(size_t count, const std::function<qint64(size_t)>& materialKey) {
    if (!m_drawOrderDirty && m_drawOrder.size() == count) return;
    m_drawOrder.resize(count);
    std::iota(m_drawOrder.begin(), m_drawOrder.end(), size_t(0));
    std::stable_sort(m_drawOrder.begin(), m_drawOrder.end(), [&](size_t a, size_t b) {
        return materialKey(a) < materialKey(b);
    });
    m_drawOrderDirty = false;
}

void Renderer::synchronizeSlot(const Scene& scene,size_t index,bool ready) {
    if(index>=m_visible.size() || index>=scene.mesh().subMeshes.size()) return;
    m_visible[index]=scene.objectVisible(int(index));
    m_drawable[index]=ready && scene.drawable(int(index));
    m_owners[index]=scene.mesh().subMeshes[index].tileOwner;
    if(m_visible[index] && m_drawable[index]) m_matrices[index]=scene.objectTransform(int(index));
}

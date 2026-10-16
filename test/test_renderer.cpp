#include "renderer.h"
#include "scene.h"
#include <gtest/gtest.h>

namespace {
class DrawState : public Renderer {
public:
    using Renderer::synchronizeSlot;
    using Renderer::resetSlots;
    using Renderer::resizeSlots;
    using Renderer::clearSlots;
    using Renderer::acceptsDraw;
    using Renderer::updateDrawOrder;
    using Renderer::m_drawOrder;
    using Renderer::m_drawOrderDirty;
    using Renderer::m_owners;
    using Renderer::m_matrices;
};
}

TEST(Renderer, VisibilityAndOwnerSelectionUseStableSlots) {
    DrawState state;
    state.resetSlots(3);
    state.m_owners={7,7,-1};
    EXPECT_TRUE(state.acceptsDraw(0,0));
    EXPECT_TRUE(state.acceptsDraw(1,0));
    EXPECT_FALSE(state.acceptsDraw(2,0));
    EXPECT_TRUE(state.acceptsDraw(2,2));
    EXPECT_FALSE(state.acceptsDraw(0,99));
    state.setSubMeshVisible(1,false);
    EXPECT_FALSE(state.acceptsDraw(1,0));
    EXPECT_FALSE(state.subMeshVisible(-1));
    EXPECT_FALSE(state.subMeshVisible(99));
}

TEST(Renderer, GrowingSlotsKeepsTransformsAndWaitsForUpload) {
    DrawState state;
    state.resetSlots(2);
    QMatrix4x4 transform; transform.translate(1,2,3);
    state.setSubMeshTransform(0,transform);
    state.setSubMeshVisible(1,false);
    state.resizeSlots(3);
    EXPECT_EQ(state.m_matrices[0],transform);
    EXPECT_FALSE(state.subMeshVisible(1));
    EXPECT_FALSE(state.subMeshVisible(2));
    state.clearSlots();
    EXPECT_FALSE(state.subMeshVisible(0));
    state.resetSlots(1);
    EXPECT_TRUE(state.subMeshVisible(0));
    EXPECT_TRUE(state.m_matrices[0].isIdentity());
}

TEST(Renderer, MaterialOrderIsStableAndRebuiltOnlyWhenInvalidated) {
    DrawState state;
    std::vector<qint64> keys{8,2,8,2};
    int comparisons=0;
    auto key=[&](size_t i) { ++comparisons; return keys[i]; };
    state.updateDrawOrder(keys.size(),key);
    EXPECT_EQ(state.m_drawOrder,(std::vector<size_t>{1,3,0,2}));
    comparisons=0;
    state.updateDrawOrder(keys.size(),key);
    EXPECT_EQ(comparisons,0);
    keys[0]=0;
    state.m_drawOrderDirty=true;
    state.updateDrawOrder(keys.size(),key);
    EXPECT_EQ(state.m_drawOrder,(std::vector<size_t>{0,1,3,2}));
    keys.push_back(-1);
    state.updateDrawOrder(keys.size(),key);
    EXPECT_EQ(state.m_drawOrder.front(),4);
}

TEST(Renderer, SceneStateAndGpuReadinessDriveCommonDrawFiltering) {
    Scene scene; Mesh mesh;
    SubMesh part; part.vertices={{0,0,0,0,0,1,0,0}}; part.indices={0,0,0};
    mesh.subMeshes.push_back(std::move(part)); scene.setMesh(std::move(mesh));
    scene.selectObject(0); scene.transformState(0).position={2,3,4};
    DrawState cache; cache.resizeSlots(1);
    cache.synchronizeSlot(scene,0,false); EXPECT_FALSE(cache.acceptsDraw(0));
    cache.synchronizeSlot(scene,0,true); EXPECT_TRUE(cache.acceptsDraw(0));
    EXPECT_EQ(cache.m_matrices[0],scene.objectTransform(0));
    scene.setObjectVisible(0,false);
    cache.synchronizeSlot(scene,0,true); EXPECT_FALSE(cache.acceptsDraw(0));
    scene.setObjectVisible(0,true); scene.transformState(0).scale={2,3,4};
    cache.synchronizeSlot(scene,0,true);
    EXPECT_EQ(cache.m_matrices[0],scene.objectTransform(0));
}

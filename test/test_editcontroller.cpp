#include <gtest/gtest.h>
#include "editcontroller.h"

namespace {
constexpr auto component=EditController::ChangePolicy::ComponentTolerance;
constexpr auto vector=EditController::ChangePolicy::VectorTolerance;
void expectVector(const QVector3D& actual,const QVector3D& expected) {
    EXPECT_NEAR((actual-expected).length(),0,1e-5f);
}
}

TEST(Editor, DragUndoRedoPreservesFullStateAndObjectIdentity) {
    EditController edit; EditTransform before,after;
    before.position={1,2,3}; after.position={4,5,6}; after.scale={2,3,4};
    after.rotation=QQuaternion::fromAxisAndAngle({0,0,1},45);
    edit.beginDrag(before); ASSERT_TRUE(edit.endDrag(7,after,component));
    auto current=after;
    EXPECT_FALSE(edit.undo(8,current)); expectVector(current.position,after.position);
    ASSERT_TRUE(edit.undo(7,current)); expectVector(current.position,before.position);
    EXPECT_EQ(current.rotation,before.rotation); expectVector(current.scale,before.scale);
    ASSERT_TRUE(edit.redo(7,current)); expectVector(current.position,after.position);
    EXPECT_EQ(current.rotation,after.rotation); expectVector(current.scale,after.scale);
}

TEST(Editor, NoOpKeepsRedoAndNewEditDiscardsRedo) {
    EditController edit; EditTransform before,after; after.position={1,0,0};
    ASSERT_TRUE(edit.commit(0,before,after,component));
    auto state=after; ASSERT_TRUE(edit.undo(0,state));
    edit.beginDrag(state); EXPECT_FALSE(edit.endDrag(0,state,component)); EXPECT_TRUE(edit.canRedo());
    auto next=state; next.position={2,0,0};
    EXPECT_TRUE(edit.endDrag(0,next,component)); EXPECT_FALSE(edit.canRedo());
    edit.clearHistory(); EXPECT_FALSE(edit.canUndo()); EXPECT_FALSE(edit.canRedo());
}

TEST(Editor, LegacyBackendsKeepTheirDifferentNoOpThresholds) {
    EditTransform before,after; after.scale+=QVector3D(8e-6f,8e-6f,8e-6f);
    EditController gl,vk;
    EXPECT_FALSE(gl.commit(0,before,after,component));
    EXPECT_TRUE(vk.commit(0,before,after,vector));
    after=before; after.rotation=QQuaternion(1,1e-6f,0,0);
    gl.clearHistory(); vk.clearHistory();
    EXPECT_FALSE(gl.commit(0,before,after,component));
    EXPECT_TRUE(vk.commit(0,before,after,vector));
}

TEST(Editor, PivotMatrixAndLocalAxesPreserveTransformOrder) {
    EditTransform state; state.position={5,0,0}; state.scale={2,2,2};
    state.rotation=QQuaternion::fromAxisAndAngle({0,0,1},90);
    const QVector3D center(10,20,30);
    const auto matrix=state.matrix(center);
    expectVector(matrix.map(center),center+state.position);
    expectVector(matrix.map(center+QVector3D(1,0,0)),center+state.position+QVector3D(0,2,0));
    expectVector(EditController::gizmoAxis(1,true,state.rotation),{0,1,0});
    expectVector(EditController::gizmoAxis(1,false,state.rotation),{1,0,0});
}

TEST(Editor, GestureMathKeepsRayAndScreenScaleSensitivity) {
    EXPECT_FLOAT_EQ(EditController::rayScale(1.5f,4,2),3);
    EXPECT_FLOAT_EQ(EditController::rayScale(1,0.001f,1),0.05f);
    EXPECT_FLOAT_EQ(EditController::rayScale(1,100,1),20);
    EXPECT_FLOAT_EQ(EditController::screenScale(1.5f,60),2.25f);
    EXPECT_FLOAT_EQ(EditController::screenScale(1,-240),0.02f);
    EXPECT_FLOAT_EQ(EditController::screenScale(99,120),100);
    EXPECT_NEAR(EditController::angleDelta(-3.1f,3.1f),0.0831853f,1e-6f);
    auto rotation=QQuaternion::fromAxisAndAngle({0,0,1},90);
    expectVector(EditController::rotate(rotation,{1,0,0},90).rotatedVector({1,0,0}),{0,0,1});
    EditTransform state; EditController::translate(state,{0,1,0},2); expectVector(state.position,{0,2,0});
}

TEST(Editor, TileStateIsSharedByOwnerAndResetRemovesHistoryAndEdits) {
    EditController edit; EditTransform state; state.position={1,2,3};
    edit.setTileState(42,state); expectVector(edit.tileState(42).position,state.position);
    expectVector(edit.tileState(43).position,{});
    edit.states().resize(2); edit.current()=state; edit.commit(0,{},state,component);
    edit.reset(); EXPECT_TRUE(edit.states().empty()); EXPECT_FALSE(edit.canUndo());
    expectVector(edit.current().scale,{1,1,1}); expectVector(edit.tileState(42).position,{});
}

TEST(Editor, DeselectAndSwitchObjectsRetainIndependentTransforms) {
    EditController edit; edit.states().resize(2); edit.selectState(0);
    edit.current().position={1,2,3}; edit.current().scale={2,3,4};
    edit.current().rotation=QQuaternion::fromAxisAndAngle({0,1,0},30);
    const auto first=edit.current();
    edit.commit(0,{},first,component);
    edit.selectState(-1);
    EXPECT_FALSE(edit.canUndo()); expectVector(edit.states()[0].position,first.position);
    edit.selectState(1); expectVector(edit.current().scale,{1,1,1});
    edit.current().position={8,0,0};
    edit.selectState(0); expectVector(edit.current().position,first.position);
    expectVector(edit.current().scale,first.scale); EXPECT_EQ(edit.current().rotation,first.rotation);
    edit.selectState(1); expectVector(edit.current().position,{8,0,0});
}

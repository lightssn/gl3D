#include <gtest/gtest.h>
#include "camera.h"
#include "renderscene.h"
#include "tilenode.h"

TEST(Camera, UpAxisControlsOrbitViewAndGroundPlane) {
    Camera camera; camera.fitToSphere({3,4,5},2);
    const auto yOffset=camera.eye()-camera.target();
    EXPECT_EQ(camera.upAxis(),UpAxis::Y);
    EXPECT_EQ(camera.groundPoint(2,3),QVector3D(2,0,3));
    camera.setUpAxis(UpAxis::Z);
    const auto zOffset=camera.eye()-camera.target();
    EXPECT_NEAR(zOffset.x(),yOffset.x(),0.0001);
    EXPECT_NEAR(zOffset.y(),-yOffset.z(),0.0001);
    EXPECT_NEAR(zOffset.z(),yOffset.y(),0.0001);
    EXPECT_EQ(camera.groundPoint(2,3),QVector3D(2,3,0));
    const auto targetInView=camera.viewMatrix().map(camera.target());
    EXPECT_NEAR(targetInView.x(),0,0.0001); EXPECT_NEAR(targetInView.y(),0,0.0001);
    EXPECT_NEAR(targetInView.z(),-camera.distance(),0.0001);
    camera.orbit(0.8f,0);
    EXPECT_NEAR((camera.eye()-camera.target()).z(),zOffset.z(),0.0001);
}

TEST(Camera, ZUpPanStaysOnGroundAndKeyboardElevationUsesZ) {
    Camera camera; camera.setUpAxis(UpAxis::Z); camera.fitToSphere({3,4,5},2);
    camera.pan(100,80); EXPECT_FLOAT_EQ(camera.target().z(),5);
    camera.panLocal(1,0,1); EXPECT_FLOAT_EQ(camera.target().z(),5);
    const auto before=camera.target(); camera.panLocal(0,1,0);
    EXPECT_FLOAT_EQ(camera.target().x(),before.x()); EXPECT_FLOAT_EQ(camera.target().y(),before.y());
    EXPECT_GT(camera.target().z(),before.z());
}

TEST(Camera, LoadingSceneRestoresFormatDefaultAxis) {
    Mesh tiled; tiled.tileRoot=std::make_shared<TileNode>();
    tiled.tileRoot->radius=1; tiled.bboxMin={-1,-1,-1}; tiled.bboxMax={1,1,1};
    RenderScene scene; scene.setMesh(tiled);
    auto& camera=scene.cameraController().camera();
    EXPECT_EQ(camera.upAxis(),UpAxis::Z);
    camera.setUpAxis(UpAxis::Y); scene.setMesh(tiled); EXPECT_EQ(camera.upAxis(),UpAxis::Z);
    Mesh ordinary; ordinary.subMeshes.emplace_back(); scene.setMesh(ordinary);
    EXPECT_EQ(camera.upAxis(),UpAxis::Y);
}

#include <gtest/gtest.h>
#include "camera.h"
#include "scene.h"
#include "tilenode.h"
#include "renderframe.h"

TEST(Camera, GizmoScreenSizeIgnoresZoomDepthAndViewportSize) {
    for(const QSize size : {QSize(800,600),QSize(1600,900)})
    for(bool perspective : {false,true}) for(float distance : {1.0f,10.0f,1000.0f}) {
        QMatrix4x4 view,projection;
        view.translate(0,0,-distance);
        if(perspective) projection.perspective(45,float(size.width())/size.height(),0.01f,10000);
        else projection.ortho(-distance,distance,-distance,distance,0.01f,10000);
        const QVector3D center(0.1f,0.2f,-distance*0.25f);
        const QMatrix4x4 matrix=makeGizmoMatrix(view,projection,center,size);
        const QVector3D origin=matrix.map(QVector3D());
        const QVector3D x=matrix.map(QVector3D(1,0,0));
        const QVector3D y=matrix.map(QVector3D(0,1,0));
        EXPECT_NEAR((x.x()-origin.x())*size.width()*0.5f,GizmoPixelRadius,0.001f);
        EXPECT_NEAR((y.y()-origin.y())*size.height()*0.5f,GizmoPixelRadius,0.001f);
        const QVector3D projectedCenter=(projection*view).map(center);
        EXPECT_NEAR(origin.x(),projectedCenter.x(),0.0001f);
        EXPECT_NEAR(origin.y(),projectedCenter.y(),0.0001f);
    }
}

TEST(Camera, GizmoBehindCameraIsHidden) {
    QMatrix4x4 view,projection;
    projection.perspective(45,1,0.1f,100);
    const auto matrix=makeGizmoMatrix(view,projection,{0,0,1},{800,600});
    EXPECT_EQ(matrix*QVector4D(1,0,0,1),QVector4D());
}

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
    Scene scene; scene.setMesh(tiled);
    auto& camera=scene.cameraController().camera();
    EXPECT_EQ(camera.upAxis(),UpAxis::Z);
    camera.setUpAxis(UpAxis::Y); scene.setMesh(tiled); EXPECT_EQ(camera.upAxis(),UpAxis::Z);
    Mesh ordinary; ordinary.subMeshes.emplace_back(); scene.setMesh(ordinary);
    EXPECT_EQ(camera.upAxis(),UpAxis::Y);
}

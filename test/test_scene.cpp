#include "scene.h"
#include "tilenode.h"
#include <QTimer>
#include <gtest/gtest.h>
#include <utility>

namespace {
class TestView : public Scene {
public:
    QTimer timer;
    int redraws=0, selections=0;
    bool undoEnabled=false, redoEnabled=false;
    int loadsStarted=0, loadsFinished=0, inputsReset=0;
    std::vector<int> progress;
    QString loadedInfo, loadError, unavailable;

    RenderBackend backend() const override { return RenderBackend::OpenGL; }
    DebugSnapshot debugSnapshot() override { return {}; }
    void requestRedraw() override { ++redraws; invalidateTiles(); }
protected:
    void notifyLoadStarted() override { ++loadsStarted; }
    void notifyLoadProgress(int percent) override { progress.push_back(percent); }
    void notifyMeshLoaded(const QString& info) override { loadedInfo=info; }
    void notifyLoadFailed(const QString& error) override { loadError=error; }
    void notifyLoadFinished() override { ++loadsFinished; }
    void resetInputState() override { ++inputsReset; }
    QString meshAvailabilityError() const override { return unavailable; }
    QTimer* redrawTimer() override { return &timer; }
    void notifyHistoryChanged(bool undo,bool redo) override { undoEnabled=undo; redoEnabled=redo; }
    void notifySelectionChanged(int,bool changed) override { if(changed) ++selections; }
};
Mesh ordinaryMesh() {
    Mesh mesh;
    for(int i=0;i<2;++i) {
        SubMesh part;
        part.vertices={{0,0,0,0,0,1,0,0},{1,0,0,0,0,1,1,0},{0,1,0,0,0,1,0,1}};
        part.indices={0,1,2};
        mesh.subMeshes.push_back(std::move(part));
    }
    mesh.bboxMin={0,0,0}; mesh.bboxMax={1,1,0};
    return mesh;
}
}

TEST(Scene, SelectionUndoRedoAndVisibilityUseSceneOwnedState) {
    TestView view;
    view.setMesh(ordinaryMesh());
    view.selectSubMesh(0);
    EXPECT_EQ(&view.editor().current(),&view.transformState(0));
    view.beginEdit();
    auto& state=view.transformState(0);
    state.position={2,3,4}; state.scale={2,3,4};
    state.rotation=QQuaternion::fromAxisAndAngle({0,1,0},30);
    const auto edited=state;
    view.finishEdit(EditController::ChangePolicy::ComponentTolerance);
    EXPECT_TRUE(view.undoEnabled);
    view.undo();
    EXPECT_EQ(state.position,QVector3D()); EXPECT_EQ(state.scale,QVector3D(1,1,1));
    EXPECT_TRUE(view.redoEnabled);
    view.redo();
    EXPECT_EQ(state.position,edited.position); EXPECT_EQ(state.rotation,edited.rotation); EXPECT_EQ(state.scale,edited.scale);
    view.setSubMeshVisible(0,false); EXPECT_FALSE(view.subMeshVisible(0));
    view.selectSubMesh(1); view.editor().current().position={8,0,0};
    view.selectSubMesh(-1); view.selectSubMesh(0);
    EXPECT_EQ(view.editor().current().position,edited.position);
    EXPECT_EQ(view.transformState(1).position,QVector3D(8,0,0));
    EXPECT_FALSE(view.canUndo()); EXPECT_FALSE(view.subMeshVisible(0));
    view.clear(); EXPECT_EQ(view.subMeshCount(),0); EXPECT_FALSE(view.canRedo());
}

TEST(Scene, UndoDuringDragPreservesLiveRedoState) {
    TestView view; view.setMesh(ordinaryMesh()); view.selectSubMesh(0);
    view.beginEdit(); view.editor().current().position={1,0,0};
    view.finishEdit(EditController::ChangePolicy::VectorTolerance);
    view.beginEdit(); view.editor().current().position={3,0,0};
    view.undo(); EXPECT_EQ(view.editor().current().position,QVector3D());
    view.redo(); EXPECT_EQ(view.editor().current().position,QVector3D(3,0,0));
}

TEST(Scene, TileOwnersShareEditsAndUndoAcrossGeometrySlots) {
    TestView view;
    Mesh mesh=ordinaryMesh();
    mesh.tileRoot=std::make_shared<TileNode>(); mesh.tileRoot->owner=42; mesh.tileRoot->radius=2;
    mesh.tileRoot->geometry=std::move(mesh.subMeshes);
    view.setMesh(std::move(mesh));
    ASSERT_EQ(view.subMeshCount(),2);
    EXPECT_EQ(&view.transformState(0),&view.transformState(1));
    view.selectSubMesh(0); view.beginEdit(); view.editor().current().position={4,0,0};
    view.finishEdit(EditController::ChangePolicy::ComponentTolerance);
    EXPECT_EQ(view.subMeshTransform(0),view.subMeshTransform(1));
    EXPECT_NEAR(view.subMeshTransform(1).map(QVector3D()).x(),4,1e-5f);
    view.undo(); EXPECT_TRUE(view.subMeshTransform(1).isIdentity());
    view.redo(); EXPECT_NEAR(view.subMeshTransform(1).map(QVector3D()).x(),4,1e-5f);
    view.selectSubMesh(1); EXPECT_EQ(view.editor().current().position,QVector3D(4,0,0));
    view.setSubMeshVisible(1,false); EXPECT_FALSE(view.subMeshVisible(0));
    view.setPbr(true); view.setNormalMap(true);
    EXPECT_FALSE(view.options().pbr); EXPECT_FALSE(view.options().normalMap);
}

TEST(Scene, CommonOptionsMarkChangesAndUpdateTimer) {
    TestView view; view.setMesh(ordinaryMesh());
    view.setWireframe(true); view.setDepthTest(false); view.setFaceCulling(true);
    EXPECT_TRUE(view.changes().drawOptions);
    view.setUpAxis(UpAxis::Z); view.setTileBounds(true); EXPECT_TRUE(view.changes().grid);
    view.setOrtho(true); EXPECT_TRUE(view.cameraController().camera().ortho());
    view.setPbr(true); view.setNormalMap(false); view.setDebugView(10); view.setOutlineWidth(-1);
    EXPECT_TRUE(view.options().pbr); EXPECT_FALSE(view.options().normalMap);
    EXPECT_EQ(view.options().debugView,2); EXPECT_EQ(view.outlineWidth(),0);
    view.setTargetFps(20); view.setFixedFpsEnabled(true);
    EXPECT_TRUE(view.timer.isActive()); EXPECT_EQ(view.timer.interval(),50);
    view.setFixedFpsEnabled(false); EXPECT_FALSE(view.timer.isActive());
    view.setTransformMode(99); EXPECT_EQ(view.transformMode(),3);
    view.setAntialiasing(false); EXPECT_FALSE(view.options().antialiasing);
    view.setMipmaps(true); EXPECT_TRUE(view.mipmapsEnabled());
    view.setClearColor(0.2f,0.3f,0.4f); EXPECT_FLOAT_EQ(view.clearColor()[1],0.3f);
    const int before=view.selections; view.selectSubMesh(100); EXPECT_EQ(view.selections,before);
}

TEST(Editor, CurrentStateSurvivesSlotGrowthAndOwnerReassignment) {
    EditController edit; edit.resizeObjects(2); edit.selectState(0); edit.current().position={1,2,3};
    edit.resizeObjects(4096);
    EXPECT_EQ(&edit.current(),&edit.state(0)); EXPECT_EQ(edit.current().position,QVector3D(1,2,3));
    edit.setOwner(0,42); edit.current().scale={2,3,4};
    edit.setOwner(1,42); EXPECT_EQ(&edit.state(0),&edit.state(1));
    edit.setOwner(0,99); edit.setOwner(2,42); edit.selectState(2);
    EXPECT_EQ(edit.current().scale,QVector3D(2,3,4));
    EXPECT_EQ(edit.state(0).scale,QVector3D(1,1,1));
}

TEST(Scene, BoundsAndEditCenterSurviveCpuGeometryRelease) {
    Scene scene; scene.setMesh(ordinaryMesh());
    EXPECT_EQ(scene.mesh().subMeshes[0].boundsMin,QVector3D(0,0,0));
    EXPECT_EQ(scene.mesh().subMeshes[0].boundsMax,QVector3D(1,1,0));
    scene.mesh().releaseGeometry();
    EXPECT_EQ(scene.mesh().subMeshes[0].boundsMax,QVector3D(1,1,0));
    scene.selectObject(0); scene.transformState(0).scale={2,2,2};
    EXPECT_EQ(scene.objectTransform(0).map(QVector3D(0.5f,0.5f,0)),QVector3D(0.5f,0.5f,0));
}

TEST(Scene, TileDisplayCacheFollowsTransformVisibilityAndReplacement) {
    Scene scene;
    auto makeTile=[] {
        Mesh mesh=ordinaryMesh();
        mesh.tileRoot=std::make_shared<TileNode>();
        mesh.tileRoot->owner=42; mesh.tileRoot->radius=2;
        for(auto& part:mesh.subMeshes) { part.boundsMin={0,0,0}; part.boundsMax={1,1,0}; }
        mesh.tileRoot->geometry=std::move(mesh.subMeshes);
        return mesh;
    };
    scene.setMesh(makeTile());
    scene.options().showTileBounds=true;
    for(int i=0;i<scene.subMeshCount();++i) scene.markUploaded(i);
    scene.updateTiles({800,600});
    EXPECT_EQ(scene.tileRepresentative(1),0);
    ASSERT_FALSE(scene.tileBoundsLines().empty());
    auto initial=scene.tileBoundsLines();
    scene.selectSubMesh(0); scene.beginEdit(); scene.editor().current().position={0.1f,0,0};
    scene.finishEdit(EditController::ChangePolicy::ComponentTolerance);
    scene.updateTiles({800,600});
    ASSERT_EQ(scene.tileBoundsLines().size(),initial.size());
    EXPECT_EQ(scene.tileBoundsLines()[0],initial[0]+QVector3D(0.1f,0,0));
    scene.setSubMeshVisible(0,false); scene.updateTiles({800,600});
    EXPECT_TRUE(scene.tileBoundsLines().empty());
    scene.clear(); EXPECT_TRUE(scene.tileBoundsLines().empty());
    scene.setMesh(ordinaryMesh());
    EXPECT_EQ(scene.tileRepresentative(1),1); EXPECT_TRUE(scene.tileBoundsLines().empty());
}

TEST(Scene, SharedChangesCoalesceAndKeepIndependentWork) {
    Scene scene; scene.setMesh(ordinaryMesh());
    EXPECT_TRUE(scene.changes().mesh);
    scene.changes()={};
    scene.setAntialiasing(false);
    EXPECT_FALSE(scene.changes().drawOptions);
    scene.setMipmaps(true); scene.setMipmaps(false);
    scene.setWireframe(true); scene.setDepthTest(false);
    scene.setClearColor(0.2f,0.3f,0.4f); scene.setUpAxis(UpAxis::Z);
    EXPECT_TRUE(scene.changes().textures);
    EXPECT_TRUE(scene.changes().drawOptions);
    EXPECT_TRUE(scene.changes().grid);
    EXPECT_FALSE(scene.changes().mesh);
    EXPECT_TRUE(std::exchange(scene.changes().textures,false));
    EXPECT_FALSE(scene.changes().textures);
    EXPECT_TRUE(scene.changes().grid);
    scene.setMipmaps(true);
    EXPECT_TRUE(scene.changes().textures);
    scene.selectSubMesh(0); scene.beginEdit(); scene.editor().current().position={1,2,3};
    scene.finishEdit(EditController::ChangePolicy::ComponentTolerance);
    scene.selectSubMesh(-1);
    scene.setSubMeshVisible(1,false);
    const auto objects=std::exchange(scene.changes().objects,{});
    EXPECT_EQ(objects,(std::set<int>{0,1}));
    EXPECT_TRUE(scene.changes().objects.empty());
    scene.setSubMeshVisible(0,false);
    EXPECT_EQ(scene.changes().objects,(std::set<int>{0}));
    scene.clear();
    EXPECT_TRUE(scene.changes().mesh); EXPECT_TRUE(scene.changes().tileBounds);
    EXPECT_TRUE(scene.changes().objects.empty());
    scene.setMesh(ordinaryMesh());
    EXPECT_TRUE(scene.changes().mesh); EXPECT_TRUE(scene.changes().grid);
}

TEST(Scene, LoadStagesSeparateReadingFromUploadAndMoveGeometry) {
    TestView view;
    EXPECT_FALSE(view.beginLoad({}));
    ASSERT_TRUE(view.beginLoad({QStringLiteral("keyboard.glb")}));
    EXPECT_TRUE(view.isLoading()); EXPECT_EQ(view.loadsStarted,1);
    EXPECT_FALSE(view.beginLoad({QStringLiteral("another.glb")}));
    view.finishUpload(); EXPECT_TRUE(view.isLoading()); EXPECT_EQ(view.loadsFinished,0);
    view.reportLoadProgress(50); EXPECT_EQ(view.progress.back(),45);
    auto loaded=std::make_shared<Mesh>(ordinaryMesh());
    const auto* vertices=loaded->subMeshes[0].vertices.data();
    view.acceptLoadResult(true,{},loaded);
    EXPECT_TRUE(view.isLoading()); EXPECT_EQ(view.progress.back(),90);
    EXPECT_EQ(view.mesh().subMeshes[0].vertices.data(),vertices);
    EXPECT_TRUE(loaded->subMeshes.empty());
    EXPECT_TRUE(view.loadedInfo.startsWith(QStringLiteral("keyboard.glb")));
    EXPECT_EQ(view.sourcePath(),QStringLiteral("keyboard.glb"));
    EXPECT_EQ(view.loadsFinished,0); EXPECT_EQ(view.inputsReset,1);
    view.reportLoadProgress(30); EXPECT_EQ(view.progress.back(),90);
    view.finishUpload(); EXPECT_FALSE(view.isLoading());
    EXPECT_EQ(view.progress.back(),100); EXPECT_EQ(view.loadsFinished,1);
    view.finishUpload(); EXPECT_EQ(view.loadsFinished,1);
}

TEST(Scene, FailedOrUnavailableLoadPreservesExistingMesh) {
    TestView view; view.setMesh(ordinaryMesh(),QStringLiteral("old.glb"));
    ASSERT_TRUE(view.beginLoad({QStringLiteral("missing.glb")}));
    view.acceptLoadResult(false,QStringLiteral("missing"),{});
    EXPECT_FALSE(view.isLoading()); EXPECT_EQ(view.loadError,QStringLiteral("missing"));
    EXPECT_EQ(view.sourcePath(),QStringLiteral("old.glb")); EXPECT_EQ(view.subMeshCount(),2);
    ASSERT_TRUE(view.beginLoad({QStringLiteral("new.glb")}));
    view.unavailable=QStringLiteral("context unavailable");
    auto loaded=std::make_shared<Mesh>(ordinaryMesh());
    view.acceptLoadResult(true,{},loaded);
    EXPECT_FALSE(view.isLoading()); EXPECT_EQ(view.loadError,view.unavailable);
    EXPECT_EQ(view.sourcePath(),QStringLiteral("old.glb"));
    EXPECT_EQ(loaded->subMeshes.size(),2u); EXPECT_EQ(view.loadsFinished,2);
    view.unavailable.clear();
    ASSERT_TRUE(view.beginLoad({QStringLiteral("null.glb")}));
    view.acceptLoadResult(true,{},{});
    EXPECT_FALSE(view.isLoading()); EXPECT_FALSE(view.loadError.isEmpty());
}

TEST(Scene, ClearDiscardsLateReadAndCancelsPendingUpload) {
    TestView view;
    ASSERT_TRUE(view.beginLoad({QStringLiteral("old.glb")}));
    view.clear(); EXPECT_FALSE(view.isLoading()); EXPECT_EQ(view.loadsFinished,1);
    EXPECT_FALSE(view.beginLoad({QStringLiteral("new.glb")}));
    view.acceptLoadResult(true,{},std::make_shared<Mesh>(ordinaryMesh()));
    EXPECT_FALSE(view.hasMesh()); EXPECT_EQ(view.loadsFinished,1);
    ASSERT_TRUE(view.beginLoad({QStringLiteral("new.glb")}));
    view.acceptLoadResult(true,{},std::make_shared<Mesh>(ordinaryMesh()));
    EXPECT_TRUE(view.isLoading()); EXPECT_TRUE(view.hasMesh());
    view.clear(); view.finishUpload();
    EXPECT_FALSE(view.isLoading()); EXPECT_FALSE(view.hasMesh());
    EXPECT_EQ(view.loadsFinished,2);
}

TEST(Scene, DirectSetMeshAppliesDefaultsAndResetsEditingNotifications) {
    TestView view; view.setMesh(ordinaryMesh());
    view.selectSubMesh(0); view.beginEdit(); view.editor().current().position={1,0,0};
    view.finishEdit(EditController::ChangePolicy::ComponentTolerance);
    EXPECT_TRUE(view.undoEnabled);
    view.options().pbr=true; view.options().normalMap=true; view.options().outlineWidth=42;
    Mesh mesh=ordinaryMesh();
    mesh.tileRoot=std::make_shared<TileNode>(); mesh.tileRoot->owner=42; mesh.tileRoot->radius=2;
    mesh.tileRoot->geometry=std::move(mesh.subMeshes);
    view.setMesh(std::move(mesh));
    EXPECT_FALSE(view.options().pbr); EXPECT_FALSE(view.options().normalMap);
    EXPECT_EQ(view.options().outlineWidth,RenderOptions{}.outlineWidth);
    EXPECT_EQ(view.selectedSubMesh(),-1); EXPECT_FALSE(view.undoEnabled); EXPECT_FALSE(view.redoEnabled);
    EXPECT_FALSE(view.isLoading()); EXPECT_EQ(view.loadsStarted,0);
}

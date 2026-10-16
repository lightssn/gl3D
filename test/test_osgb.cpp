#include <gtest/gtest.h>
#include "osgbloader.h"
#include "meshloader.h"
#include "scene.h"
#include "processmemory.h"
#include <QTemporaryDir>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QThread>
#include <QElapsedTimer>
#include <QThreadPool>
#include <cmath>
#include <osgDB/WriteFile>
#include <osgDB/Registry>
#include <QCoreApplication>
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/Texture2D>
#include <osg/Material>

namespace {
osg::ref_ptr<osg::Geode> triangle(bool textured=false) {
    osg::ref_ptr<osg::Geometry> geometry=new osg::Geometry;
    osg::ref_ptr<osg::Vec3Array> positions=new osg::Vec3Array;
    positions->push_back({-1,-1,0}); positions->push_back({1,-1,0}); positions->push_back({0,1,0});
    geometry->setVertexArray(positions);
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    osg::ref_ptr<osg::Material> material=new osg::Material;
    material->setDiffuse(osg::Material::FRONT_AND_BACK,{0.2f,0.4f,0.6f,1});
    geometry->getOrCreateStateSet()->setAttribute(material);
    if(textured) {
        osg::ref_ptr<osg::Image> image=new osg::Image;
        image->allocateImage(2,2,1,GL_RGBA,GL_UNSIGNED_BYTE);
        for(int i=0;i<16;++i) image->data()[i]=static_cast<unsigned char>(i*16);
        osg::ref_ptr<osg::Texture2D> texture=new osg::Texture2D(image);
        geometry->getOrCreateStateSet()->setTextureAttributeAndModes(0,texture);
        osg::ref_ptr<osg::Vec2Array> uv=new osg::Vec2Array;
        uv->push_back({0,0}); uv->push_back({1,0}); uv->push_back({0.5,1});
        geometry->setTexCoordArray(0,uv);
    }
    osg::ref_ptr<osg::Geode> geode=new osg::Geode;
    geode->setName("tile"); geode->addDrawable(geometry);
    return geode;
}

QString writeNode(const QTemporaryDir& dir,const QString& name,const osg::Node& node) {
    const QString path=dir.filePath(name);
    auto& plugins=osgDB::Registry::instance()->getLibraryFilePathList();
    const auto directory=(QCoreApplication::applicationDirPath()+"/osgPlugins-3.6.5").toStdString();
    if(std::find(plugins.begin(),plugins.end(),directory)==plugins.end()) plugins.push_back(directory);
    EXPECT_TRUE(osgDB::writeNodeFile(node,path.toUtf8().toStdString()));
    return path;
}

std::shared_ptr<TileNode> pageTree(const QTemporaryDir& dir,bool missing=false) {
    if(!missing) writeNode(dir,"detail.osgb",*triangle(true));
    osg::ref_ptr<osg::PagedLOD> lod=new osg::PagedLOD;
    lod->setCenterMode(osg::LOD::USER_DEFINED_CENTER); lod->setCenter({0,0,0}); lod->setRadius(2);
    lod->addChild(triangle(),10,1000000);
    lod->setFileName(1,"detail.osgb"); lod->setRange(1,0,10);
    Mesh mesh; QString error;
    EXPECT_TRUE(OsgbLoader::load(QStringList{writeNode(dir,"root.osgb",*lod)},mesh,&error)) << error.toStdString();
    return mesh.tileRoot;
}

bool waitPage(TileStream& stream,Mesh& mesh,const Camera& camera) {
    QElapsedTimer elapsed; elapsed.start();
    while(elapsed.elapsed()<3000) {
        stream.update(mesh,camera,{800,600},-1);
        if(mesh.subMeshes.size()>1 || stream.stats().failures) return mesh.subMeshes.size()>1;
        QThread::msleep(5);
    }
    return false;
}
}

TEST(Osgb, ReadsTextureMaterialAndNormals) {
    QTemporaryDir dir;
    Mesh mesh; QString error;
    ASSERT_TRUE(OsgbLoader::load(QStringList{writeNode(dir,QString::fromUtf8("纹理.osgb"),*triangle(true))},mesh,&error)) << error.toStdString();
    ASSERT_TRUE(mesh.tileRoot);
    ASSERT_EQ(mesh.tileRoot->geometry.size(),1);
    const auto& part=mesh.tileRoot->geometry[0];
    EXPECT_EQ(part.indices.size(),3); EXPECT_EQ(part.vertices.size(),3);
    EXPECT_FALSE(part.baseImage.isNull()); EXPECT_EQ(part.baseImage.width(),2);
    EXPECT_FLOAT_EQ(part.diffuseColor.y(),0.4f);
    EXPECT_NEAR(part.vertices[0].nz,1.f,0.001f);
}

TEST(Osgb, LargeCoordinatesUseLocalOrigin) {
    QTemporaryDir dir;
    osg::ref_ptr<osg::MatrixTransform> transform=new osg::MatrixTransform;
    transform->setMatrix(osg::Matrixd::translate(10000000,20000000,30000000));
    transform->addChild(triangle());
    Mesh mesh; ASSERT_TRUE(OsgbLoader::load(QStringList{writeNode(dir,"translated.osgb",*transform)},mesh,nullptr));
    ASSERT_EQ(mesh.tileRoot->children.size(),1);
    const auto& part=mesh.tileRoot->children[0]->geometry[0];
    EXPECT_NEAR(part.vertices[1].px-part.vertices[0].px,2.f,0.001f);
    EXPECT_LT(std::abs(part.vertices[0].pz),10.f);
}

TEST(TileStream, FrustumRejectsOutsideSphere) {
    QMatrix4x4 projection; projection.perspective(45,1,0.1,100);
    EXPECT_TRUE(TileStream::sphereVisible(projection,{0,0,-5},1));
    EXPECT_FALSE(TileStream::sphereVisible(projection,{100,0,-5},1));
    EXPECT_FALSE(TileStream::sphereVisible(projection,{0,0,5},1));
}

TEST(Osgb, OnDemandWaitsForPagingAndUploadsThenSleepsUntilChanges) {
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir);
    Scene scene; scene.setMesh(std::move(mesh));
    EXPECT_TRUE(scene.options().osgbOnDemand); EXPECT_TRUE(scene.needsTileRedraw());
    scene.updateTiles({800,600}); EXPECT_TRUE(scene.needsTileRedraw());
    for(int frame=0;frame<20;++frame) {
        QThreadPool::globalInstance()->waitForDone(); scene.updateTiles({800,600});
        for(int s=0;s<int(scene.mesh().subMeshes.size());++s)
            if(scene.mesh().subMeshes[size_t(s)].resident && !scene.mesh().subMeshes[size_t(s)].uploaded) scene.markUploaded(s);
    }
    ASSERT_EQ(scene.mesh().subMeshes.size(),2);
    EXPECT_FALSE(scene.needsTileRedraw());
    scene.cameraController().camera().panLocal(0.01f,0,0);
    EXPECT_TRUE(scene.needsTileRedraw()); scene.updateTiles({800,600});
    EXPECT_FALSE(scene.needsTileRedraw());
    scene.invalidateTiles(); EXPECT_TRUE(scene.needsTileRedraw());
    scene.updateTiles({800,600}); EXPECT_FALSE(scene.needsTileRedraw());
    scene.options().osgbOnDemand=false; EXPECT_TRUE(scene.needsTileRedraw());
    scene.options().osgbOnDemand=true; EXPECT_FALSE(scene.needsTileRedraw());
    scene.setTileVisible(0,false); EXPECT_TRUE(scene.needsTileRedraw());
    scene.updateTiles({800,600}); EXPECT_FALSE(scene.needsTileRedraw());
    scene.clear(); EXPECT_FALSE(scene.needsTileRedraw());
}

TEST(Osgb, SharedBoundsFollowVisibleGeometryAndTileTransforms) {
    Mesh mesh; mesh.tileRoot=std::make_shared<TileNode>();
    mesh.tileRoot->owner=0; mesh.tileRoot->radius=2;
    SubMesh part; part.vertices.resize(3); part.indices={0,1,2};
    part.boundsMin={-1,-1,0}; part.boundsMax={1,1,0};
    mesh.tileRoot->geometry.push_back(part); mesh.bboxMin={-2,-2,-2}; mesh.bboxMax={2,2,2};
    Scene scene; scene.setMesh(std::move(mesh)); scene.markUploaded(0);
    scene.options().showTileBounds=true; scene.updateTiles({800,600});
    ASSERT_EQ(scene.tileBoundsLines().size(),24);
    EXPECT_EQ(scene.tileBoundsLines()[0],QVector3D(-1,-1,0));
    const auto revision=scene.tileBoundsRevision();
    QMatrix4x4 matrix; matrix.translate(1,0,0); scene.setTileTransform(0,matrix);
    scene.updateTiles({800,600});
    EXPECT_EQ(scene.tileBoundsLines()[0],QVector3D(0,-1,0));
    EXPECT_GT(scene.tileBoundsRevision(),revision);
    scene.setTileVisible(0,false); scene.updateTiles({800,600});
    EXPECT_TRUE(scene.tileBoundsLines().empty());
}

TEST(TileStream, KeepsCoarseUntilDetailUploadedAndEvictsUnusedPage) {
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir);
    TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1);
    ASSERT_EQ(mesh.subMeshes.size(),1); stream.uploaded(mesh,0);
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_TRUE(mesh.subMeshes[0].drawable);
    ASSERT_TRUE(waitPage(stream,mesh,camera));
    EXPECT_TRUE(mesh.subMeshes[0].drawable);
    EXPECT_FALSE(mesh.subMeshes[1].uploaded);
    stream.uploaded(mesh,1); stream.update(mesh,camera,{800,600},-1);
    EXPECT_FALSE(mesh.subMeshes[0].drawable); EXPECT_TRUE(mesh.subMeshes[1].drawable);
    stream.budgetEviction=true; stream.budgetBytes=0; camera.zoom(-20); QThread::msleep(2);
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_TRUE(mesh.subMeshes[0].drawable); EXPECT_FALSE(mesh.subMeshes[1].resident);
    EXPECT_GE(stream.stats().evicted,1);
}

TEST(TileStream, MissingPageKeepsCoarseAndReportsFailure) {
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir,true);
    ASSERT_TRUE(mesh.tileRoot);
    TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1); stream.uploaded(mesh,0);
    EXPECT_FALSE(waitPage(stream,mesh,camera));
    EXPECT_GE(stream.stats().failures,1); EXPECT_TRUE(mesh.subMeshes[0].drawable);
    EXPECT_FALSE(stream.stats().lastError.isEmpty());
}

TEST(TileStream, EditingAndVisibilityApplyToFutureLodPages) {
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir);
    ASSERT_TRUE(mesh.tileRoot);
    TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1); stream.uploaded(mesh,0);
    const int owner=mesh.subMeshes[0].tileOwner;
    QMatrix4x4 edit; edit.translate(0.1f,0,0); edit.rotate(15,{0,1,0});
    stream.setTransform(owner,edit);
    ASSERT_TRUE(waitPage(stream,mesh,camera));
    EXPECT_EQ(mesh.subMeshes[1].tileOwner,owner); EXPECT_EQ(stream.transform(owner),edit);
    stream.uploaded(mesh,1); stream.setVisible(owner,false);
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_FALSE(mesh.subMeshes[0].drawable); EXPECT_FALSE(mesh.subMeshes[1].drawable);
}

TEST(Osgb, MergesSameMaterialWithinNode) {
    QTemporaryDir dir; auto geode=triangle(); auto second=triangle();
    geode->addDrawable(second->getDrawable(0));
    Mesh mesh; ASSERT_TRUE(OsgbLoader::load(QStringList{writeNode(dir,"merged.osgb",*geode)},mesh,nullptr));
    ASSERT_EQ(mesh.tileRoot->geometry.size(),1);
    EXPECT_EQ(mesh.tileRoot->geometry[0].indices.size(),6);
}

TEST(TileStream, CancellationDoesNotWaitOnFutureDestruction) {
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir);
    ASSERT_TRUE(mesh.tileRoot);
    auto stream=std::make_unique<TileStream>(mesh);
    Camera camera; camera.fitToSphere({0,0,0},1); stream->uploaded(mesh,0);
    stream->update(mesh,camera,{800,600},-1);
    QElapsedTimer timer; timer.start(); stream.reset(); EXPECT_LT(timer.elapsed(),100);
    QThreadPool::globalInstance()->waitForDone();
}


TEST(Osgb, ExternalPageInheritsParentMaterialAndTexture) {
    QTemporaryDir dir;
    auto detail = triangle();
    detail->getDrawable(0)->setStateSet(nullptr);
    writeNode(dir,"detail.osgb",*detail);
    auto source = triangle(true);
    osg::ref_ptr<osg::PagedLOD> lod = new osg::PagedLOD;
    lod->setStateSet(source->getDrawable(0)->getStateSet());
    lod->setCenterMode(osg::LOD::USER_DEFINED_CENTER); lod->setCenter({0,0,0}); lod->setRadius(2);
    lod->addChild(triangle(),10,1000000);
    lod->setFileName(1,"detail.osgb"); lod->setRange(1,0,10);
    Mesh mesh;
    ASSERT_TRUE(OsgbLoader::load(QStringList{writeNode(dir,"root.osgb",*lod)},mesh,nullptr));
    TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1); stream.uploaded(mesh,0);
    ASSERT_TRUE(waitPage(stream,mesh,camera));
    EXPECT_FLOAT_EQ(mesh.subMeshes[1].diffuseColor.y(),0.4f);
    EXPECT_FALSE(mesh.subMeshes[1].baseImage.isNull());
    EXPECT_EQ(mesh.subMeshes[0].baseImage.cacheKey(),mesh.subMeshes[1].baseImage.cacheKey());
}

TEST(TileStream, RejectsReferenceToRootFile) {
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir);
    mesh.tileRoot->children[1]->file=dir.filePath("root.osgb");
    TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1); stream.uploaded(mesh,0);
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_EQ(stream.stats().failures,1); EXPECT_EQ(stream.stats().loading,0);
    EXPECT_TRUE(mesh.subMeshes[0].drawable);
}

TEST(TileStream, PixelLodKeepsCoarseUntilDetailReady) {
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir);
    mesh.tileRoot->pixelRange=true;
    mesh.tileRoot->children[0]->minRange=0; mesh.tileRoot->children[0]->maxRange=100;
    mesh.tileRoot->children[1]->minRange=100; mesh.tileRoot->children[1]->maxRange=1000000;
    TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1); stream.uploaded(mesh,0);
    ASSERT_TRUE(waitPage(stream,mesh,camera)); EXPECT_TRUE(mesh.subMeshes[0].drawable);
    stream.uploaded(mesh,1); stream.update(mesh,camera,{800,600},-1);
    EXPECT_TRUE(mesh.subMeshes[1].drawable); EXPECT_FALSE(mesh.subMeshes[0].drawable);
    EXPECT_TRUE(stream.levels()[1].pixelRange);
    stream.setLevel(1,true,100000,1000000,true);
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_TRUE(mesh.subMeshes[0].drawable); EXPECT_FALSE(mesh.subMeshes[1].drawable);
}

TEST(Osgb, SceneCopiesDoNotConsumeSharedSourceTree) {
    QTemporaryDir dir; Mesh mesh;
    ASSERT_TRUE(OsgbLoader::load(QStringList{writeNode(dir,"source.osgb",*triangle())},mesh,nullptr));
    Scene first, second; first.setMesh(mesh); second.setMesh(mesh);
    ASSERT_EQ(mesh.tileRoot->geometry.size(),1);
    ASSERT_EQ(first.mesh().subMeshes.size(),1); ASSERT_EQ(second.mesh().subMeshes.size(),1);
    EXPECT_EQ(first.mesh().subMeshes[0].vertices.size(),3);
    EXPECT_EQ(second.mesh().subMeshes[0].vertices.size(),3);
    EXPECT_NE(first.mesh().tileRoot,second.mesh().tileRoot);
}


TEST(TileStream, ReloadReusesEvictedSlotWithNewRevision) {
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir);
    TileStream stream(mesh); Camera nearCamera; nearCamera.fitToSphere({0,0,0},1);
    stream.uploaded(mesh,0); ASSERT_TRUE(waitPage(stream,mesh,nearCamera));
    const auto revision=mesh.subMeshes[1].revision;
    stream.uploaded(mesh,1); stream.update(mesh,nearCamera,{800,600},-1);
    Camera farCamera=nearCamera; farCamera.zoom(-20); stream.budgetEviction=true; stream.budgetBytes=0;
    QThread::msleep(2); stream.update(mesh,farCamera,{800,600},-1);
    ASSERT_FALSE(mesh.subMeshes[1].resident);
    QElapsedTimer timer; timer.start();
    while (timer.elapsed()<3000 && !mesh.subMeshes[1].resident) {
        stream.update(mesh,nearCamera,{800,600},-1); QThread::msleep(5);
    }
    ASSERT_TRUE(mesh.subMeshes[1].resident); EXPECT_EQ(mesh.subMeshes.size(),2);
    EXPECT_GT(mesh.subMeshes[1].revision,revision);
    EXPECT_FALSE(mesh.subMeshes[1].uploaded); EXPECT_TRUE(mesh.subMeshes[0].drawable);
    stream.uploaded(mesh,1); stream.update(mesh,nearCamera,{800,600},-1);
    EXPECT_TRUE(mesh.subMeshes[1].drawable); EXPECT_FALSE(mesh.subMeshes[0].drawable);
    for(int cycle=0;cycle<3;++cycle) {
        const auto previous=mesh.subMeshes[1].revision;
        QThread::msleep(2); stream.update(mesh,farCamera,{800,600},-1);
        ASSERT_FALSE(mesh.subMeshes[1].resident);
        timer.restart();
        while(timer.elapsed()<3000 && !mesh.subMeshes[1].resident) {
            stream.update(mesh,nearCamera,{800,600},-1); QThread::msleep(5);
        }
        ASSERT_TRUE(mesh.subMeshes[1].resident); EXPECT_GT(mesh.subMeshes[1].revision,previous);
        stream.uploaded(mesh,1); stream.update(mesh,nearCamera,{800,600},-1);
        EXPECT_TRUE(mesh.subMeshes[1].drawable); EXPECT_FALSE(mesh.subMeshes[0].drawable);
    }
}


TEST(Osgb, MultipleFilesPreserveRelativeCoordinatesAndIndependentOwners) {
    QTemporaryDir dir;
    osg::ref_ptr<osg::MatrixTransform> first=new osg::MatrixTransform;
    osg::ref_ptr<osg::MatrixTransform> second=new osg::MatrixTransform;
    first->setMatrix(osg::Matrixd::translate(10000000,20000000,30000000));
    second->setMatrix(osg::Matrixd::translate(10000100,20000000,30000000));
    first->addChild(triangle()); second->addChild(triangle());
    const QStringList paths{writeNode(dir,"first.osgb",*first),writeNode(dir,"second.osgb",*second)};
    Mesh mesh; QString error; int percent=0;
    MeshLoader worker;
    bool loaded=false;
    QObject::connect(&worker,&MeshLoader::progress,[&](int value){percent=value;});
    QObject::connect(&worker,&MeshLoader::finished,[&](bool ok,const QString& message,std::shared_ptr<Mesh> result){
        loaded=ok; error=message;
        if(result) mesh=std::move(*result);
    });
    worker.load(paths);
    ASSERT_TRUE(loaded)<<error.toStdString();
    EXPECT_EQ(percent,100);
    Scene scene; scene.setMesh(std::move(mesh));
    ASSERT_EQ(scene.mesh().subMeshes.size(),2);
    const auto& a=scene.mesh().subMeshes[0]; const auto& b=scene.mesh().subMeshes[1];
    EXPECT_NE(a.tileOwner,b.tileOwner);
    EXPECT_NEAR(b.vertices[0].px-a.vertices[0].px,100,0.001);
    EXPECT_LT(std::abs(a.vertices[0].px),100);
    QMatrix4x4 edit; edit.translate(4,0,0); scene.setTileTransform(0,edit);
    EXPECT_EQ(scene.tileTransform(0),edit); EXPECT_EQ(scene.tileTransform(1),QMatrix4x4());
}

TEST(Osgb, MultipleRootsKeepSeparateRelativePagePaths) {
    QTemporaryDir first,second; ASSERT_TRUE(pageTree(first)); ASSERT_TRUE(pageTree(second));
    Mesh mesh;
    ASSERT_TRUE(OsgbLoader::load({first.filePath("root.osgb"),second.filePath("root.osgb")},mesh));
    ASSERT_EQ(mesh.tileRoot->children.size(),2);
    EXPECT_NE(mesh.tileRoot->children[0]->children[1]->file,mesh.tileRoot->children[1]->children[1]->file);
    TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1);
    ASSERT_EQ(mesh.subMeshes.size(),2); stream.uploaded(mesh,0); stream.uploaded(mesh,1);
    QElapsedTimer timer; timer.start();
    while(timer.elapsed()<3000 && mesh.subMeshes.size()<4) {
        stream.update(mesh,camera,{800,600},-1); QThread::msleep(5);
    }
    ASSERT_EQ(mesh.subMeshes.size(),4); EXPECT_EQ(stream.stats().failures,0);
    std::map<int,int> owners; for(const auto& part:mesh.subMeshes) ++owners[part.tileOwner];
    ASSERT_EQ(owners.size(),2); for(const auto& owner:owners) EXPECT_EQ(owner.second,2);
}

TEST(Osgb, RejectsMixedMultipleSelection) {
    MeshLoader worker;
    bool received=false;
    QObject::connect(&worker,&MeshLoader::finished,[&](bool ok,const QString& error,std::shared_ptr<Mesh> result){
        received=true;
        EXPECT_FALSE(ok); EXPECT_FALSE(error.isEmpty()); EXPECT_FALSE(result);
    });
    worker.load({"first.osgb","second.obj"});
    EXPECT_TRUE(received);
}


TEST(Osgb, IndexBuilderFindsRootsSkipsEmptyAndKeepsPagePaths) {
    for(bool forest:{false,true}) {
        QTemporaryDir dataDir,outputDir;
        ASSERT_TRUE(pageTree(dataDir));
        osg::ref_ptr<osg::Group> empty=new osg::Group; writeNode(dataDir,"empty.osgb",*empty);
        if(forest) writeNode(dataDir,"independent.osgb",*triangle());
        const QString output=outputDir.filePath("index.osgb");
        QString program=QCoreApplication::applicationDirPath()+"/gl3d_osgb_index";
#ifdef Q_OS_WIN
        program+=".exe";
#endif
        QProcess process; process.start(program,{dataDir.path(),output,"--same-directory"});
        ASSERT_TRUE(process.waitForFinished(15000));
        ASSERT_EQ(process.exitStatus(),QProcess::NormalExit);
        ASSERT_EQ(process.exitCode(),0)<<process.readAllStandardError().constData();
        QFile report(output+".json"); ASSERT_TRUE(report.open(QIODevice::ReadOnly));
        const auto json=QJsonDocument::fromJson(report.readAll()).object();
        EXPECT_EQ(json["roots"].toArray().size(),forest?2:1);
        EXPECT_EQ(json["emptyFiles"].toArray().size(),1);
        EXPECT_TRUE(json["missingPages"].toArray().isEmpty());
        // Stage the portable entry beside its tiles, then move the complete directory.
        ASSERT_TRUE(QFile::copy(output,dataDir.filePath("index.osgb")));
        const QString moved=outputDir.filePath("moved");
        ASSERT_TRUE(QDir().rename(dataDir.path(),moved));
        Mesh mesh; ASSERT_TRUE(OsgbLoader::load(QStringList{moved+"/index.osgb"},mesh,nullptr));
        if(!forest) EXPECT_TRUE(mesh.tileRoot->lod);
        TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1);
        const size_t initial=mesh.subMeshes.size();
        for(size_t i=0;i<initial;++i) stream.uploaded(mesh,int(i));
        QElapsedTimer timer; timer.start();
        while(timer.elapsed()<3000 && mesh.subMeshes.size()==initial) {
            stream.update(mesh,camera,{800,600},-1); QThread::msleep(5);
        }
        EXPECT_GT(mesh.subMeshes.size(),initial); EXPECT_EQ(stream.stats().failures,0);
    }
}

TEST(TileStream, LevelRangesAndVisibilityControlSelection) {
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir);
    TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1);
    stream.uploaded(mesh,0);
    ASSERT_EQ(stream.levels().size(),2);
    EXPECT_FLOAT_EQ(stream.levels()[1].maxRange,10);
    ASSERT_TRUE(waitPage(stream,mesh,camera)); stream.uploaded(mesh,1);
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_TRUE(mesh.subMeshes[1].drawable); EXPECT_FALSE(mesh.subMeshes[0].drawable);
    stream.setLevel(1,false,0,10,false);
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_FALSE(mesh.subMeshes[1].drawable); EXPECT_TRUE(mesh.subMeshes[0].drawable);
    stream.setLevel(0,false,10,1000000,false);
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_FALSE(mesh.subMeshes[0].drawable); EXPECT_FALSE(mesh.subMeshes[1].drawable);
    stream.resetLevels();
    stream.setLevel(1,true,0,0.1f,true);
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_TRUE(mesh.subMeshes[0].drawable); EXPECT_FALSE(mesh.subMeshes[1].drawable);
    stream.resetLevels(); stream.update(mesh,camera,{800,600},-1);
    EXPECT_TRUE(mesh.subMeshes[1].drawable);
    EXPECT_FALSE(stream.levels()[1].overridden);
}

TEST(TileStream, HiddenIntermediateLevelStillPagesFinerLevel) {
    QTemporaryDir dir;
    writeNode(dir,"finest.osgb",*triangle());
    osg::ref_ptr<osg::PagedLOD> middle=new osg::PagedLOD;
    middle->setCenterMode(osg::LOD::USER_DEFINED_CENTER); middle->setCenter({0,0,0}); middle->setRadius(2);
    middle->addChild(triangle(),5,1000000);
    middle->setFileName(1,"finest.osgb"); middle->setRange(1,0,5);
    Mesh mesh; mesh.tileRoot=pageTree(dir);
    writeNode(dir,"detail.osgb",*middle);
    TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1); stream.uploaded(mesh,0);
    stream.setLevel(1,false,0,10,false);
    QElapsedTimer timer; timer.start();
    while(timer.elapsed()<3000 && mesh.subMeshes.size()<3) {
        stream.update(mesh,camera,{800,600},-1);
        for(int i=0;i<int(mesh.subMeshes.size());++i) if(!mesh.subMeshes[size_t(i)].uploaded) stream.uploaded(mesh,i);
        QThread::msleep(5);
    }
    ASSERT_EQ(mesh.subMeshes.size(),3); ASSERT_EQ(stream.levels().size(),3);
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_FALSE(mesh.subMeshes[1].drawable); EXPECT_TRUE(mesh.subMeshes[2].drawable);
    EXPECT_FALSE(stream.levels()[1].enabled);
}

TEST(TileStream, RequiredPageSurvivesBudgetPressureBeforeGpuUpload) {
    QTemporaryDir dir; Mesh mesh;
    auto page=pageTree(dir);
    mesh.tileRoot=std::make_shared<TileNode>(); mesh.tileRoot->children.push_back(page);
    // Keep traversal busy after the requested page to cross millisecond ticks.
    for(int i=0;i<15000;++i) mesh.tileRoot->children.push_back(std::make_shared<TileNode>());
    TileStream stream(mesh); stream.budgetBytes=0; stream.budgetEviction=true;
    Camera camera; camera.fitToSphere({0,0,0},1); stream.uploaded(mesh,0);
    QElapsedTimer timer; timer.start();
    while(timer.elapsed()<3000 && mesh.subMeshes.size()<2) {
        stream.update(mesh,camera,{800,600},-1); QThread::msleep(2);
    }
    ASSERT_GE(mesh.subMeshes.size(),2);
    ASSERT_TRUE(mesh.subMeshes[1].resident);
    const auto revision=mesh.subMeshes[1].revision;
    for(int i=0;i<5;++i) {
        QThread::msleep(2); stream.update(mesh,camera,{800,600},-1);
        EXPECT_TRUE(mesh.subMeshes[1].resident);
        EXPECT_EQ(mesh.subMeshes[1].revision,revision);
    }
    stream.uploaded(mesh,1); stream.update(mesh,camera,{800,600},-1);
    EXPECT_TRUE(mesh.subMeshes[1].drawable);
    EXPECT_EQ(stream.stats().evicted,0);
}

TEST(Osgb, LevelTintUsesStableSharedPaletteAndStreamedGeometryLevel) {
    EXPECT_EQ(Scene::tileLevelColor(0),QVector3D(0.65f,1.f,0.65f));
    EXPECT_EQ(Scene::tileLevelColor(5),QVector3D(1.f,0.65f,0.65f));
    EXPECT_EQ(Scene::tileLevelColor(9),Scene::tileLevelColor(5));
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir);
    Scene scene; scene.setMesh(std::move(mesh));
    EXPECT_EQ(scene.mesh().subMeshes[0].tileLevel,0);
    EXPECT_EQ(scene.tileTint(0),QVector4D());
    scene.options().tileLevelTint=true;
    EXPECT_EQ(scene.tileTint(0),QVector4D(Scene::tileLevelColor(0),0.45f));
    scene.markUploaded(0);
    QElapsedTimer timer; timer.start();
    while(timer.elapsed()<3000 && scene.mesh().subMeshes.size()<2) {
        scene.updateTiles({800,600}); QThread::msleep(5);
    }
    ASSERT_EQ(scene.mesh().subMeshes.size(),2);
    EXPECT_EQ(scene.mesh().subMeshes[1].tileLevel,1);
    EXPECT_EQ(scene.tileTint(1),QVector4D(Scene::tileLevelColor(1),0.45f));
    scene.options().tileLevelTint=false; EXPECT_EQ(scene.tileTint(1),QVector4D());
}

namespace {
struct BlockPageWorkers {
    QThreadPool* pool=QThreadPool::globalInstance();
    int previous=pool->maxThreadCount();
    std::promise<void> release, started;
    BlockPageWorkers() {
        pool->waitForDone(); pool->setMaxThreadCount(1);
        auto gate=release.get_future().share();
        pool->start([this,gate] { started.set_value(); gate.wait(); });
        started.get_future().wait();
    }
    ~BlockPageWorkers() { release.set_value(); pool->waitForDone(); pool->setMaxThreadCount(previous); }
};
std::vector<std::shared_ptr<TileNode>> focusPages(const QTemporaryDir& dir,Mesh& mesh,const Camera& camera) {
    auto source=pageTree(dir)->children[1];
    mesh.tileRoot=std::make_shared<TileNode>();
    const QVector3D right=camera.viewMatrix().row(0).toVector3D();
    const float halfWidth=camera.distance()*std::tan(camera.fov()*0.0087266463f);
    for(float offset:{0.9f,0.7f,0.5f,0.3f,0.1f,0.f}) {
        auto page=std::make_shared<TileNode>(*source); page->radius=0.01f;
        page->center=camera.target()+right*halfWidth*offset;
        mesh.tileRoot->children.push_back(page);
    }
    return mesh.tileRoot->children;
}
}
TEST(TileStream, FourJobsPrioritizeScreenCenterAndRebuildPendingQueue) {
    QTemporaryDir dir; Mesh mesh; Camera camera; camera.fitToSphere({0,0,0},1);
    auto pages=focusPages(dir,mesh,camera);
    BlockPageWorkers workers;
    TileStream stream(mesh); stream.update(mesh,camera,{600,600},-1);
    EXPECT_EQ(stream.stats().loading,4); EXPECT_EQ(stream.stats().queued,2);
    for(int i=0;i<6;++i) EXPECT_EQ(pages[size_t(i)]->loading,i>=2);
    // Pan far enough to move every original page out of the frustum.
    Camera moved=camera; moved.panLocal(100,0,0);
    stream.update(mesh,moved,{600,600},-1);
    EXPECT_EQ(stream.stats().queued,0);
}
TEST(TileStream, OffscreenCompletedResultsAreDiscardedWithoutFailureAndCanReload) {
    QTemporaryDir dir; Mesh mesh; Camera camera; camera.fitToSphere({0,0,0},1);
    auto pages=focusPages(dir,mesh,camera);
    TileStream stream(mesh); stream.update(mesh,camera,{600,600},-1);
    QThreadPool::globalInstance()->waitForDone();
    Camera moved=camera; moved.panLocal(100,0,0);
    stream.update(mesh,moved,{600,600},-1);
    EXPECT_TRUE(mesh.subMeshes.empty()); EXPECT_EQ(stream.stats().loading,0);
    EXPECT_EQ(stream.stats().failures,0); EXPECT_EQ(stream.stats().queued,0);
    for(auto& page:pages) { EXPECT_FALSE(page->loading); EXPECT_FALSE(page->loaded); EXPECT_FALSE(page->failed); }
    stream.update(mesh,camera,{600,600},-1);
    EXPECT_EQ(stream.stats().loading,4);
    QThreadPool::globalInstance()->waitForDone(); stream.update(mesh,camera,{600,600},-1);
    EXPECT_GE(mesh.subMeshes.size(),4); EXPECT_EQ(stream.stats().failures,0);
}

TEST(TileStream, DefaultCacheRetainsPagesUntilBudgetEvictionIsEnabled) {
    QTemporaryDir dir; Mesh mesh; mesh.tileRoot=pageTree(dir);
    TileStream stream(mesh); EXPECT_EQ(stream.budgetBytes,1024ll*1024*1024); EXPECT_FALSE(stream.budgetEviction);
    Camera near; near.fitToSphere({0,0,0},1); stream.uploaded(mesh,0);
    ASSERT_TRUE(waitPage(stream,mesh,near)); stream.uploaded(mesh,1);
    Camera far=near; far.zoom(-20); stream.budgetBytes=0;
    for(int frame=0;frame<5;++frame) { QThread::msleep(2); stream.update(mesh,far,{800,600},-1); }
    EXPECT_TRUE(mesh.subMeshes[1].resident); EXPECT_EQ(stream.stats().evicted,0);
    stream.budgetEviction=true; stream.budgetBytes=1024ll*1024*1024;
    stream.update(mesh,far,{800,600},-1); EXPECT_TRUE(mesh.subMeshes[1].resident);
    stream.budgetBytes=0; stream.update(mesh,far,{800,600},-1);
    EXPECT_FALSE(mesh.subMeshes[1].resident); EXPECT_GT(stream.stats().evicted,0);
}
TEST(TileStream, RetainedIndexLoadsDetailWithoutReadingEvictedAncestor) {
    QTemporaryDir dir;
    writeNode(dir,"leaf.osgb",*triangle(true));
    osg::ref_ptr<osg::PagedLOD> middle=new osg::PagedLOD;
    middle->setCenterMode(osg::LOD::USER_DEFINED_CENTER); middle->setCenter({0,0,0}); middle->setRadius(2);
    middle->addChild(triangle(),5,1000000);
    middle->setFileName(1,"leaf.osgb"); middle->setRange(1,0,5);
    writeNode(dir,"detail.osgb",*middle);
    // pageTree normally writes detail; retain our nested page instead.
    Mesh mesh; mesh.tileRoot=pageTree(dir,true);
    TileStream stream(mesh); Camera camera; camera.fitToSphere({0,0,0},1);
    for(int i=0;i<20;++i) {
        stream.update(mesh,camera,{800,600},-1); QThreadPool::globalInstance()->waitForDone();
        for(int s=0;s<int(mesh.subMeshes.size());++s)
            if(mesh.subMeshes[size_t(s)].resident) stream.uploaded(mesh,s);
    }
    auto parent=mesh.tileRoot->children[1];
    ASSERT_TRUE(parent->loaded); ASSERT_EQ(parent->children.size(),2);
    auto leaf=parent->children[1]; ASSERT_TRUE(leaf->loaded);
    // Make the parent older: depth must still win over LRU ordering.
    parent->lastUsed=0; leaf->lastUsed=100000;
    Camera far=camera; far.zoom(-20);
    stream.budgetEviction=true;
    stream.budgetBytes=TileStream::memoryUsage(mesh).bytes-1;
    stream.update(mesh,far,{800,600},-1);
    EXPECT_TRUE(parent->loaded); EXPECT_FALSE(leaf->loaded);
    stream.budgetBytes=0; stream.update(mesh,far,{800,600},-1);
    EXPECT_FALSE(parent->loaded); EXPECT_TRUE(parent->indexed);
    ASSERT_EQ(parent->children.size(),2); EXPECT_EQ(parent->children[1],leaf);
    ASSERT_TRUE(QFile::remove(dir.filePath("detail.osgb")));
    stream.budgetEviction=false;
    stream.update(mesh,camera,{800,600},-1);
    EXPECT_TRUE(leaf->loading); EXPECT_FALSE(parent->loading);
    QThreadPool::globalInstance()->waitForDone(); stream.update(mesh,camera,{800,600},-1);
    ASSERT_TRUE(leaf->loaded); EXPECT_FALSE(parent->loaded); EXPECT_FALSE(parent->loading); EXPECT_EQ(stream.stats().failures,0);
    for(int s:leaf->renderSlots) stream.uploaded(mesh,s);
    stream.update(mesh,camera,{800,600},-1);
    ASSERT_FALSE(leaf->renderSlots.empty());
    EXPECT_TRUE(mesh.subMeshes[size_t(leaf->renderSlots[0])].drawable);
    // Restore an intermediate tier without losing its already resident leaf.
    writeNode(dir,"detail.osgb",*middle);
    Camera intermediate; intermediate.fitToSphere({0,0,0},3);
    stream.update(mesh,intermediate,{800,600},-1);
    ASSERT_TRUE(parent->loading);
    QThreadPool::globalInstance()->waitForDone(); stream.update(mesh,intermediate,{800,600},-1);
    EXPECT_TRUE(parent->loaded); EXPECT_EQ(parent->children[1],leaf);
    EXPECT_TRUE(leaf->loaded); EXPECT_EQ(stream.stats().failures,0);
}

TEST(TileStream, CpuAndGpuMemoryCountSharedImagesOnce) {
    Mesh mesh; QImage image(8,8,QImage::Format_RGBA8888); image.fill(Qt::red);
    mesh.subMeshes.resize(2);
    for(auto& part:mesh.subMeshes) {
        part.baseImage=image; part.uploaded=true; part.savedVertices=3; part.savedTriangles=1;
    }
    const auto shared=TileStream::memoryUsage(mesh);
    EXPECT_EQ(shared.cpuBytes,qint64(mesh.subMeshes.capacity()*sizeof(SubMesh))+image.sizeInBytes());
    EXPECT_EQ(shared.gpuBytes,2*(3*qint64(sizeof(Vertex))+3*qint64(sizeof(unsigned)))+image.sizeInBytes());
    mesh.subMeshes[1].baseImage=image.copy(); const auto separate=TileStream::memoryUsage(mesh);
    EXPECT_EQ(separate.cpuBytes-shared.cpuBytes,image.sizeInBytes());
    EXPECT_EQ(separate.gpuBytes-shared.gpuBytes,image.sizeInBytes());
}
TEST(Osgb, BackgroundConversionReportsKnownDataSeparately) {
    QTemporaryDir dir; auto root=pageTree(dir); auto page=root->children[1];
    auto memory=std::make_shared<PageMemory>(); QString error;
    auto result=OsgbLoader::readPage(page->file,page->sourceTransform,page->origin,&error,{},page->inheritedDiffuse,page->inheritedImage,memory);
    ASSERT_TRUE(result); EXPECT_FALSE(memory->reading.load()); EXPECT_GT(memory->sourceBytes.load(),0);
    EXPECT_GT(memory->convertedBytes.load(),0);
}
TEST(ProcessMemory, WindowsCountersUseOperatingSystemMeasurements) {
#ifdef Q_OS_WIN
    const auto usage=processMemory(); ASSERT_TRUE(usage.valid);
    EXPECT_GT(usage.workingSet,0); EXPECT_GT(usage.privateCommit,0);
    EXPECT_TRUE(usage.privateWorkingSetValid); EXPECT_GT(usage.privateWorkingSet,0);
    EXPECT_LE(usage.privateWorkingSet,usage.workingSet+8*1024*1024);
#endif
}

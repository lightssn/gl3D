#include <gtest/gtest.h>
#include "osgbloader.h"
#include "renderscene.h"
#include <QTemporaryDir>
#include <QThread>
#include <QElapsedTimer>
#include <QThreadPool>
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
    EXPECT_TRUE(OsgbLoader::load(writeNode(dir,"root.osgb",*lod),mesh,&error)) << error.toStdString();
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
    ASSERT_TRUE(OsgbLoader::load(writeNode(dir,QString::fromUtf8("纹理.osgb"),*triangle(true)),mesh,&error)) << error.toStdString();
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
    Mesh mesh; ASSERT_TRUE(OsgbLoader::load(writeNode(dir,"translated.osgb",*transform),mesh,nullptr));
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
    stream.expireMs=0; camera.zoom(-20); QThread::msleep(2);
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
    Mesh mesh; ASSERT_TRUE(OsgbLoader::load(writeNode(dir,"merged.osgb",*geode),mesh,nullptr));
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
    ASSERT_TRUE(OsgbLoader::load(writeNode(dir,"root.osgb",*lod),mesh,nullptr));
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
}

TEST(Osgb, SceneCopiesDoNotConsumeSharedSourceTree) {
    QTemporaryDir dir; Mesh mesh;
    ASSERT_TRUE(OsgbLoader::load(writeNode(dir,"source.osgb",*triangle()),mesh,nullptr));
    RenderScene first, second; first.setMesh(mesh); second.setMesh(mesh);
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
    Camera farCamera=nearCamera; farCamera.zoom(-20); stream.expireMs=0;
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
}

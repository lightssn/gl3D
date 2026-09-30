#include "osgbstream.h"
#include <gtest/gtest.h>
#include <QFileInfo>
#include <chrono>
#include <thread>

TEST(OsgbStreamTest, PackagedTileLoadsAndStreams)
{
#ifndef GL3D_HAS_OSGB
    GTEST_SKIP() << "OSG support is unavailable";
#else
    const QString folder = QStringLiteral(GL3D_TEST_SOURCE_DIR "/data/osgb/tile_17_24");
    if (!QFileInfo::exists(folder)) GTEST_SKIP() << "OSGB fixture is unavailable";

    OsgbStream stream;
    QString error;
    ASSERT_TRUE(stream.open(folder, &error)) << error.toStdString();
    EXPECT_TRUE(stream.available());
    EXPECT_GT(stream.stats().tiles, 1);
    EXPECT_GT(stream.rootRadius(), 0);

    Camera camera;
    camera.fitToSphere(stream.rootCenter(), stream.rootRadius());
    const QSize viewport(1280, 720);
    bool sawGeometry = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        stream.update(camera, viewport);
        sawGeometry |= !stream.visibleMesh().subMeshes.empty();
        if (sawGeometry && stream.stats().pending == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    EXPECT_TRUE(sawGeometry);
    EXPECT_EQ(stream.stats().failed, 0);
    EXPECT_GT(stream.stats().tiles, 2);
    EXPECT_GT(stream.stats().cacheBytes, 0);
    EXPECT_EQ(stream.tileIds().size(), stream.visibleMesh().subMeshes.size());

    OsgbStream topLevel;
    ASSERT_TRUE(topLevel.open(QStringLiteral(GL3D_TEST_SOURCE_DIR "/data/osgb"), &error))
        << error.toStdString();
    EXPECT_GT(topLevel.stats().tiles, 2);
#endif
}

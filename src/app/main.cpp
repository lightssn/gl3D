#include "mainwindow.h"
#include "meshloader.h"

#include <QApplication>
#include <QFont>
#include <QFileInfo>
#include <QSurfaceFormat>
#include <QCommandLineParser>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

// 命令行模式 仅打印模型信息不进GUI
static int runInfo(const QString& path) {
    Mesh mesh;
    QString err;
    if (!MeshLoader::load(path, mesh, &err)) {
        fprintf(stderr, "[gl3d] 加载失败: %s\n", qPrintable(err));
        return 1;
        }
    printf("[gl3d] %s\n", qPrintable(path));
    printf("  顶点:     %d\n", mesh.vertexCount());
    printf("  三角形:   %d\n", mesh.triangleCount());
    printf("  子网格:   %d\n", (int)mesh.subMeshes.size());
    for (const auto& s : mesh.subMeshes)
        printf("    - %s  三角%zu  纹理%s\n",
               s.materialName.empty() ? "(默认)" : s.materialName.c_str(),
               s.indices.size() / 3,
               s.texturePath.isEmpty() ? "无" : qPrintable(QFileInfo(s.texturePath).fileName()));
    QVector3D mn = mesh.bboxMin, mx = mesh.bboxMax;
    printf("  包围盒:   (%.3f %.3f %.3f) ~ (%.3f %.3f %.3f)\n",
           mn.x(), mn.y(), mn.z(), mx.x(), mx.y(), mx.z());
    return 0;
    }

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); // 控制台中文输出
#endif

    QApplication app(argc, argv);
    app.setApplicationName("gl3d");
    app.setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("gl3d 模型查看器 stl/obj → OpenGL渲染");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption infoOpt({"i", "info"}, "仅打印模型信息 不启动GUI");
    parser.addOption(infoOpt);
    parser.addPositionalArgument("model", "模型文件路径 stl/obj");
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (parser.isSet(infoOpt)) {
        if (args.isEmpty()) {
            fprintf(stderr, "[gl3d] --info 需要模型路径\n");
            return 1;
            }
        return runInfo(args.first());
        }

#ifdef _WIN32
    app.setFont(QFont("Microsoft YaHei", 9));
#endif

    // 全局GL格式 4.6核心+深度+stencil+4xMSAA
    QSurfaceFormat fmt;
    fmt.setVersion(4, 6);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8); //选中描边用stencil
    fmt.setSamples(4);
    QSurfaceFormat::setDefaultFormat(fmt);

    MainWindow w;
    w.show();
    if (!args.isEmpty())
        w.openModel(args.first()); // CLI传入模型直接加载
    return app.exec();
    }

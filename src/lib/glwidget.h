#pragma once

#include "gl3d_export.h"
#include "camera.h"
#include "mesh.h"
#include <QOpenGLWidget>
#include <QElapsedTimer>
#include <memory>

class Shader;//前向声明，cpp再引入头文件，改动后编译更快
class MeshRenderer;

//OpenGL渲染视口 负责context生命周期/渲染循环/键鼠交互
//交互 左键旋转 右键平移 滚轮缩放 WASD平移 R复位
class GL3D_EXPORT GLWidget : public QOpenGLWidget {
        Q_OBJECT
        std::unique_ptr<Shader> m_shader;
        std::unique_ptr<MeshRenderer> m_renderer;
        Mesh m_mesh;                  //CPU侧网格 取景与统计用
        Camera m_camera;

        QPoint m_lastMousePos;
        Qt::MouseButton m_dragButton = Qt::NoButton;

        float m_clearColor[3] = { 0.13f, 0.13f, 0.16f };

        int m_frameCount = 0;         //1秒窗口内帧数
        QElapsedTimer m_fpsTimer;

    public:
        explicit GLWidget(QWidget* parent = nullptr);
        ~GLWidget() override;

        //加载模型文件 成功返回true并发出modelLoaded
        bool loadModel(const QString& path);
        void resetView();
        void setClearColor(float r, float g, float b); //主题切换联动视口底色
        bool hasModel() const;

    signals:
        void fpsUpdated(int fps);
        void modelLoaded(const QString& info);  //模型统计信息
        void loadFailed(const QString& err);

    protected:
        void initializeGL() override;
        void resizeGL(int w, int h) override;
        void paintGL() override;

        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
    };

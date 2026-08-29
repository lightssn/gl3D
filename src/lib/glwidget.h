#pragma once

#include "gl3d_export.h"
#include "camera.h"
#include "mesh.h"
#include "debugstats.h"
#include <QOpenGLWidget>
#include <QElapsedTimer>
#include <QQuaternion>
#include <QVector3D>
#include <vector>
#include <memory>

class Shader;//前向声明，cpp再引入头文件，改动后编译更快
class MeshRenderer;
class QContextMenuEvent;
class QThread;
class ModelLoaderWorker; //后台解析线程的工作对象 定义在cpp

//OpenGL渲染视口 负责context生命周期/渲染循环/键鼠交互
//交互 左键旋转+点击选中 右键点击弹菜单+右键拖拽平移 滚轮缩放 WASD平移 R复位
class GL3D_EXPORT GLWidget : public QOpenGLWidget {
        Q_OBJECT
        //操作器模式 控制模型中央显示何种手柄
        enum TransformMode { None = 0, Translate = 1, Rotate = 2, Scale = 3 };

        //纯色线段顶点 交错pos3+color3 网格/坐标轴/操作器共用
        struct OverlayVertex {
            float px, py, pz;
            float r, g, b;
        };
        struct OverlayMesh {
            unsigned int vao = 0, vbo = 0;
            int count = 0; //顶点数 以2为单位绘制GL_LINES
        };

        std::unique_ptr<Shader> m_shader;
        std::unique_ptr<Shader> m_overlayShader;
        std::unique_ptr<MeshRenderer> m_renderer;
        Mesh m_mesh;                  //CPU侧网格 取景与统计用
        Camera m_camera;

        OverlayMesh m_gridMesh;       //XY平面网格
        OverlayMesh m_axesMesh;       //左下角XYZ轴HUD
        OverlayMesh m_gizmoMesh;      //模型中央操作器
        TransformMode m_transformMode = None;
        bool m_selected = false;
        bool m_wireframe = false;     //线框模式 打开后可见三角面带浅灰边
        float m_outlineWidth = 0.01f; //选中描边沿法线挤出量

        //模型变换 T(c+pos)*R*S*T(-c) 绕模型中心旋转缩放 三模式共享
        QVector3D m_transformPos{0, 0, 0};
        QQuaternion m_transformRot;
        QVector3D m_transformScale{1, 1, 1};
        QMatrix4x4 m_modelMatrix;     //合成后的uModel 变换时刷新

        //操作器拖拽状态 按下命中轴开始 移动变换 松开结束
        int m_dragAxis = 0;           //0无 1X 2Y 3Z
        float m_prevDragParam = 0;    //平移/缩放=沿轴参数 旋转=角度
        float m_dragInitScale = 1.0f; //缩放按下时的该轴值
        QVector3D m_dragCenter;       //拖拽参考点 变换后的模型中心

        //变换历史 撤销/重做 每次拖拽前的状态入撤销栈
        struct TransformState { QVector3D pos; QQuaternion rot; QVector3D scale; };
        std::vector<TransformState> m_undoStack;
        std::vector<TransformState> m_redoStack;
        TransformState m_dragStartState; //当前拖拽起点 松开时对比是否变化

        //后台加载 解析在worker线程 解析完主线程上传显存
        QThread* m_loadThread = nullptr;
        ModelLoaderWorker* m_loadWorker = nullptr;
        bool m_loading = false; //加载中 防止重复触发
        QString m_currentPath;  //当前加载的模型路径 状态栏显示文件名用
        void onLoadFinished(bool ok, const QString& err, const Mesh& mesh);

        //离屏拾取FBO 点击处读颜色判断是否命中模型
        unsigned int m_pickFbo = 0, m_pickColor = 0, m_pickDepth = 0;
        int m_pickW = 0, m_pickH = 0;
        int m_fbW = 0, m_fbH = 0;     //视口物理尺寸
        float m_dpr = 1.0f;           //设备像素比 鼠标逻辑坐标换算

        QPoint m_lastMousePos;
        Qt::MouseButton m_dragButton = Qt::NoButton;
        int m_rightDragDist = 0;      //右键拖拽累计距离 区分点击弹菜单与拖拽平移
        int m_leftDragDist = 0;       //左键拖拽累计距离 区分点击选中与拖拽旋转

        float m_clearColor[3] = { 0.13f, 0.13f, 0.16f };

        int m_frameCount = 0;         //1秒窗口内帧数
        int m_lastFps = 0;
        QElapsedTimer m_fpsTimer;
        QElapsedTimer m_frameTimer;
        std::vector<float> m_frameTimesMs;
        bool m_antialiasing = true;
        bool m_showGrid = true;
        bool m_showGizmo = true;
        bool m_showSelection = true;
        bool m_showAxes = true;
        bool m_depthTest = true;
        bool m_faceCulling = false;
        int m_debugView = 0;
        int m_frameDrawCalls = 0;
        int m_lastDrawCalls = 0;
        DebugSnapshot collectDebugSnapshot();

        //线段几何构建 网格/坐标轴/操作器复用
        static void pushLine(std::vector<OverlayVertex>& out, QVector3D a, QVector3D b, QVector3D color);
        static void pushCone(std::vector<OverlayVertex>& out, QVector3D base, QVector3D axis, float len, float radius, QVector3D color);
        static void pushCircle(std::vector<OverlayVertex>& out, QVector3D center, QVector3D axis, float radius, QVector3D color);
        static void pushWireCube(std::vector<OverlayVertex>& out, QVector3D center, float size, QVector3D color);
        static void pushAxisArrow(std::vector<OverlayVertex>& out, QVector3D axis, float len, QVector3D color);

        void uploadOverlay(OverlayMesh& mesh, const std::vector<OverlayVertex>& verts);
        void releaseOverlay(OverlayMesh& mesh);
        void drawOverlay(const OverlayMesh& mesh, const QMatrix4x4& mvp, bool useVertexColor, const QVector3D& color);
        void buildAxesMesh();
        void rebuildGrid();
        void rebuildGizmo();
        void refreshOverlays();
        void createPickTarget(int w, int h);
        void pickAt(const QPoint& pos);
        void drawGrid(const QMatrix4x4& proj, const QMatrix4x4& view);
        void drawSelection(const QMatrix4x4& proj, const QMatrix4x4& view);
        void drawGizmo(const QMatrix4x4& proj, const QMatrix4x4& view);
        void drawAxesHud(const QMatrix4x4& view);

        //操作器拖拽 射线拾取轴/平面交点 组合模型矩阵
        void updateModelMatrix();
        void pickingRay(const QPoint& pos, QVector3D& origin, QVector3D& dir);
        QPointF projectToScreen(const QMatrix4x4& proj, const QMatrix4x4& view, const QVector3D& p);
        bool rayLineParam(const QVector3D& origin, const QVector3D& dir,
                          const QVector3D& center, const QVector3D& axis, float& param);
        bool rayPlaneInter(const QVector3D& origin, const QVector3D& dir,
                           const QVector3D& center, const QVector3D& normal, QVector3D& out);
        float gizmoAngle(const QPoint& pos, int axis); //旋转模式下鼠标在圆平面上的角度
        int gizmoHitAxis(const QPoint& pos);
        void beginGizmoDrag(const QPoint& pos, int axis);
        void dragGizmo(const QPoint& pos);

        //缩放轴随模型旋转 平移/旋转保持世界轴向
        QVector3D gizmoAxisDir(int idx) const;
        TransformState currentTransform() const;
        bool sameTransform(const TransformState& a, const TransformState& b) const;
        void applyTransform(const TransformState& s);

    public:
        explicit GLWidget(QWidget* parent = nullptr);
        ~GLWidget() override;

        //加载模型文件 成功返回true并发出modelLoaded
        bool loadModel(const QString& path);
        void resetView();
        void setClearColor(float r, float g, float b); //主题切换联动视口底色
        bool hasModel() const;
        void deleteModel();          //清除当前模型与选中状态
        void undo();                 //撤销上次变换
        void redo();                 //重做被撤销的变换
        bool canUndo() const { return !m_undoStack.empty(); }
        bool canRedo() const { return !m_redoStack.empty(); }

        void setOrtho(bool on);          //true正交 false透视
        void setTransformMode(int mode); //0无 1平移 2旋转 3缩放
        void setWireframe(bool on);      //true线框模式 可见三角面带浅灰描边
        void setAntialiasing(bool on);
        void setMipmaps(bool on);
        void setShowGrid(bool on);
        void setShowGizmo(bool on);
        void setShowSelection(bool on);
        void setShowAxes(bool on);
        void setDepthTest(bool on);
        void setFaceCulling(bool on);
        void setDebugView(int mode);
        int subMeshCount() const;
        QString subMeshName(int index) const;
        bool subMeshVisible(int index) const;
        void setSubMeshVisible(int index, bool visible);
        DebugSnapshot debugSnapshot();

    signals:
        void fpsUpdated(int fps);
        void modelLoaded(const QString& info);  //模型统计信息
        void loadFailed(const QString& err);
        void selectionChanged(bool selected); //选中状态变化 联动工具栏操作器按钮
        void contextMenuRequested(const QPoint& globalPos); //右键点击 弹出开关栏菜单
        void modelCleared(); //模型被删除 联动状态栏等
        void historyChanged(bool canUndo, bool canRedo); //撤销/重做可用性 联动按钮
        void loadStarted();       //开始加载 显示进度条
        void progressChanged(int percent); //加载进度 0~100
        void loadFinished();      //加载结束 隐藏进度条
        void debugUpdated(const DebugSnapshot& snapshot);

    protected:
        void initializeGL() override;
        void resizeGL(int w, int h) override;
        void paintGL() override;

        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;
    };

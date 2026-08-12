#pragma once

#include <QMainWindow>

class GLWidget;
class QLabel;
class QPushButton;
class QAction;
class QToolBar;
class QMenu;

// 主窗口 工具栏+GL视口+状态栏 日夜主题切换
class MainWindow : public QMainWindow {
    Q_OBJECT
    void applyTheme(bool night); // true夜 false日
    void onTransformToggled(QAction* act, bool checked); // 操作器按钮互斥切换

    GLWidget* m_glWidget = nullptr;
    QLabel* m_modelInfo = nullptr;
    QLabel* m_fpsLabel = nullptr;
    QPushButton* m_themeBtn = nullptr;
    QToolBar* m_toolBar = nullptr;   // 主工具栏 右键菜单所称菜单栏
    QAction* m_actProj = nullptr;    // 投影切换 透视/正交
    QAction* m_actUndo = nullptr;    // 撤销变换
    QAction* m_actRedo = nullptr;    // 重做变换
    QAction* m_actMove = nullptr;    // 平移操作器
    QAction* m_actRot = nullptr;     // 旋转操作器
    QAction* m_actScale = nullptr;   // 缩放操作器
    QAction* m_actDelete = nullptr;  // 删除模型
    QAction* m_curTransform = nullptr; // 当前激活的操作器按钮
    QMenu* m_contextMenu = nullptr;  // 右键菜单 开关菜单栏/状态栏
    QAction* m_actShowMenuBar = nullptr;   // 右键菜单项 顶部工具栏
    QAction* m_actShowStatusBar = nullptr; // 右键菜单项 状态栏
    bool m_night = true;

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // 打开并加载模型 供CLI参数调用
    void openModel(const QString& path);
};

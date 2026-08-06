#pragma once

#include <QMainWindow>

class GLWidget;
class QLabel;
class QPushButton;

// 主窗口 工具栏+GL视口+状态栏 日夜主题切换
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // 打开并加载模型 供CLI参数调用
    void openModel(const QString& path);

private:
    void applyTheme(bool night); // true夜 false日

    GLWidget* m_glWidget = nullptr;
    QLabel* m_modelInfo = nullptr;
    QLabel* m_fpsLabel = nullptr;
    QPushButton* m_themeBtn = nullptr;
    bool m_night = true;
};

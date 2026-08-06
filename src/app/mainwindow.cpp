#include "mainwindow.h"
#include "glwidget.h"

#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QFile>
#include <QApplication>
#include <QMessageBox>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    resize(1100, 720);
    setWindowTitle("gl3d 模型查看器");

    m_glWidget = new GLWidget(this);
    setCentralWidget(m_glWidget);

    // 工具栏 打开/复位/主题
    QToolBar* bar = addToolBar("主工具栏");
    bar->setMovable(false);
    QAction* actOpen = bar->addAction("打开模型");
    QAction* actReset = bar->addAction("复位视角");
    m_themeBtn = new QPushButton("日间模式", this);
    bar->addWidget(m_themeBtn);

    // 状态栏 模型信息+FPS
    m_modelInfo = new QLabel("未加载模型  操作: 左键旋转 右键平移 滚轮缩放 WASD平移 R复位", this);
    m_fpsLabel = new QLabel(this);
    statusBar()->addWidget(m_modelInfo, 1);
    statusBar()->addPermanentWidget(m_fpsLabel);

    connect(actOpen, &QAction::triggered, this, [this]() {
        QString path = QFileDialog::getOpenFileName(
                           this, "选择模型", QString(), "模型文件 (*.stl *.obj);;所有文件 (*)");
        if (!path.isEmpty()) openModel(path);
        });
    connect(actReset, &QAction::triggered, m_glWidget, &GLWidget::resetView);
    connect(m_themeBtn, &QPushButton::clicked, this, [this]() {
        applyTheme(!m_night);
        });

    connect(m_glWidget, &GLWidget::fpsUpdated, this, [this](int fps) {
        m_fpsLabel->setText(QString("FPS %1").arg(fps));
        });
    connect(m_glWidget, &GLWidget::modelLoaded, this, [this](const QString& info) {
        m_modelInfo->setText(info);
        });
    connect(m_glWidget, &GLWidget::loadFailed, this, [this](const QString& err) {
        QMessageBox::warning(this, "加载失败", err);
        });

    applyTheme(true); // 默认夜间主题
    }

void MainWindow::openModel(const QString& path) {
    m_glWidget->loadModel(path);
    }

// 读取exe同目录qss 应用后联动GL底色
void MainWindow::applyTheme(bool night) {
    m_night = night;
    QString file = QApplication::applicationDirPath() + (night ? "/theme_night.qss" : "/theme_day.qss");
    QFile f(file);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        qApp->setStyleSheet(QString::fromUtf8(f.readAll()));

    if (night) m_glWidget->setClearColor(0.13f, 0.13f, 0.16f);
    else m_glWidget->setClearColor(0.85f, 0.86f, 0.88f);
    m_themeBtn->setText(night ? "日间模式" : "夜间模式");
    }

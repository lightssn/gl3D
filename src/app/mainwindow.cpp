#include "mainwindow.h"
#include "glwidget.h"
#include "debugwindow.h"

#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QAction>
#include <QMenu>
#include <QFileDialog>
#include <QFile>
#include <QApplication>
#include <QMessageBox>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    resize(1100, 720);
    setWindowTitle("gl3d 模型查看器");

    m_glWidget = new GLWidget(this);
    setCentralWidget(m_glWidget);

    // 工具栏 打开/复位/主题/投影/操作器
    m_toolBar = addToolBar("主工具栏");
    m_toolBar->setMovable(false);
    QAction* actOpen = m_toolBar->addAction("打开模型");
    QAction* actReset = m_toolBar->addAction("复位视角");
    m_actDebug = m_toolBar->addAction("调试器");
    m_themeBtn = new QPushButton("日间模式", this);
    m_toolBar->addWidget(m_themeBtn);
    m_toolBar->addSeparator();

    // 投影切换 透视/正交 点击更新投影矩阵
    m_actProj = m_toolBar->addAction("投影:透视");
    m_actProj->setObjectName("projBtn");
    m_actProj->setCheckable(true);
    m_toolBar->addSeparator();

    // 线框模式 可见三角面浅灰描边
    m_actWire = m_toolBar->addAction("线框");
    m_actWire->setObjectName("wireBtn");
    m_actWire->setCheckable(true);
    connect(m_actWire, &QAction::toggled, m_glWidget, &GLWidget::setWireframe);
    m_toolBar->addSeparator();

    // 撤销/重做变换 无历史时禁用
    m_actUndo = m_toolBar->addAction("撤销");
    m_actRedo = m_toolBar->addAction("重做");
    m_actUndo->setEnabled(false);
    m_actRedo->setEnabled(false);
    m_toolBar->addSeparator();

    // 操作器按钮 互斥 仅选中模型后才显示
    m_actMove = m_toolBar->addAction("平移");
    m_actRot = m_toolBar->addAction("旋转");
    m_actScale = m_toolBar->addAction("缩放");
    m_actMove->setObjectName("transformBtn");
    m_actRot->setObjectName("transformBtn");
    m_actScale->setObjectName("transformBtn");
    m_actDelete = m_toolBar->addAction("删除");
    m_actMove->setCheckable(true);
    m_actRot->setCheckable(true);
    m_actScale->setCheckable(true);
    m_actMove->setVisible(false); //未选中模型不显示
    m_actRot->setVisible(false);
    m_actScale->setVisible(false);
    m_actDelete->setVisible(false);

    //QToolBar为QAction创建的QToolButton不继承action的objectName 需在真实按钮上设
    //QSS按id匹配 线框/平移/旋转/缩放按钮获得勾选高亮 投影按钮无按下态
    if (QWidget* b = m_toolBar->widgetForAction(m_actProj)) b->setObjectName("projBtn");
    if (QWidget* b = m_toolBar->widgetForAction(m_actWire)) b->setObjectName("wireBtn");
    for (QAction* a : { m_actMove, m_actRot, m_actScale })
        if (QWidget* b = m_toolBar->widgetForAction(a)) b->setObjectName("transformBtn");

    connect(m_actProj, &QAction::toggled, this, [this](bool on) {
        m_glWidget->setOrtho(on);
        m_actProj->setText(on ? "投影:正交" : "投影:透视");
        });
    connect(m_actMove, &QAction::toggled, this, [this](bool on) { onTransformToggled(m_actMove, on); });
    connect(m_actRot, &QAction::toggled, this, [this](bool on) { onTransformToggled(m_actRot, on); });
    connect(m_actScale, &QAction::toggled, this, [this](bool on) { onTransformToggled(m_actScale, on); });
    connect(m_actDelete, &QAction::triggered, m_glWidget, &GLWidget::deleteModel);
    connect(m_actUndo, &QAction::triggered, m_glWidget, &GLWidget::undo);
    connect(m_actRedo, &QAction::triggered, m_glWidget, &GLWidget::redo);
    // 撤销/重做可用性联动 禁用按钮由主题qss置灰
    connect(m_glWidget, &GLWidget::historyChanged, this, [this](bool canUndo, bool canRedo) {
        m_actUndo->setEnabled(canUndo);
        m_actRedo->setEnabled(canRedo);
        });

    // 状态栏 模型信息+FPS+加载进度
    m_modelInfo = new QLabel("未加载模型  操作: 左键旋转+点击选中 右键点击弹菜单/拖拽平移 滚轮缩放 WASD平移 R复位", this);
    m_fpsLabel = new QLabel(this);
    m_loadProgress = new QProgressBar(this);
    m_loadProgress->setRange(0, 100);
    m_loadProgress->setValue(0);
    m_loadProgress->setTextVisible(true);
    m_loadProgress->setFixedWidth(160);
    m_loadProgress->setVisible(false); //空闲隐藏 加载时显示
    statusBar()->addWidget(m_modelInfo, 1);
    statusBar()->addPermanentWidget(m_loadProgress);
    statusBar()->addPermanentWidget(m_fpsLabel);

    connect(actOpen, &QAction::triggered, this, [this]() {
        QString path = QFileDialog::getOpenFileName(
                           this, "选择模型", QString(), "模型文件 (*.stl *.obj);;所有文件 (*)");
        if (!path.isEmpty()) openModel(path);
        });
    connect(actReset, &QAction::triggered, m_glWidget, &GLWidget::resetView);
    m_debugWindow = new DebugWindow(m_glWidget, this);
    m_actDebug->setCheckable(true);
    connect(m_actDebug, &QAction::toggled, m_debugWindow, &QWidget::setVisible);
    connect(m_debugWindow, &QDialog::finished, m_actDebug, &QAction::setChecked);
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
    // 选中状态联动 选中显示操作器与删除按钮 取消选中隐藏并关闭操作器
    connect(m_glWidget, &GLWidget::selectionChanged, this, [this](bool selected) {
        m_actMove->setVisible(selected);
        m_actRot->setVisible(selected);
        m_actScale->setVisible(selected);
        m_actDelete->setVisible(selected);
        if (!selected && m_curTransform)
            m_curTransform->setChecked(false); //触发toggled(false) 关闭操作器
        });

    // 删除模型后复位状态栏提示
    connect(m_glWidget, &GLWidget::modelCleared, this, [this]() {
        m_modelInfo->setText("未加载模型  操作: 左键旋转+点击选中 右键点击弹菜单/拖拽平移 滚轮缩放 WASD平移 R复位");
        });

    // 加载进度条: 开始显示清零 期间更新 结束隐藏
    connect(m_glWidget, &GLWidget::loadStarted, this, [this]() {
        m_loadProgress->setValue(0);
        m_loadProgress->setVisible(true);
        m_modelInfo->setText("正在加载模型...");
        });
    connect(m_glWidget, &GLWidget::progressChanged, this, [this](int p) {
        m_loadProgress->setValue(p);
        });
    connect(m_glWidget, &GLWidget::loadFinished, this, [this]() {
        m_loadProgress->setVisible(false);
        });

    // 右键菜单 开关顶部工具栏与状态栏 视口/工具栏/状态栏共用
    m_contextMenu = new QMenu(this);
    m_actShowMenuBar = m_contextMenu->addAction("菜单栏");
    m_actShowStatusBar = m_contextMenu->addAction("状态栏");
    m_actShowMenuBar->setCheckable(true);
    m_actShowStatusBar->setCheckable(true);
    m_actShowMenuBar->setChecked(true);
    m_actShowStatusBar->setChecked(true);
    connect(m_actShowMenuBar, &QAction::toggled, this, [this](bool on) {
        m_toolBar->setVisible(on);
        });
    connect(m_actShowStatusBar, &QAction::toggled, this, [this](bool on) {
        statusBar()->setVisible(on);
        });
    // 替换工具栏自带右键菜单 避免误关工具栏后无法找回
    m_glWidget->setContextMenuPolicy(Qt::DefaultContextMenu); //视口走contextMenuEvent
    m_toolBar->setContextMenuPolicy(Qt::CustomContextMenu);
    statusBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_glWidget, &GLWidget::contextMenuRequested, this, [this](const QPoint& globalPos) {
        m_contextMenu->exec(globalPos);
        });
    connect(m_toolBar, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        m_contextMenu->exec(m_toolBar->mapToGlobal(pos));
        });
    connect(statusBar(), &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        m_contextMenu->exec(statusBar()->mapToGlobal(pos));
        });

    applyTheme(true); // 默认夜间主题
    }

void MainWindow::openModel(const QString& path) {
    m_glWidget->loadModel(path);
    }

// 操作器按钮互斥 选中新按钮时取消另两个勾选 再点当前按钮则取消
void MainWindow::onTransformToggled(QAction* act, bool checked) {
    if (checked) {
        m_curTransform = act;
        for (QAction* other : { m_actMove, m_actRot, m_actScale }) {
            if (other != act) other->setChecked(false);
            }
        int mode = (act == m_actMove) ? 1 : (act == m_actRot) ? 2 : 3;
        m_glWidget->setTransformMode(mode);
        }
    else if (m_curTransform == act) {
        m_curTransform = nullptr;
        m_glWidget->setTransformMode(0);
        }
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

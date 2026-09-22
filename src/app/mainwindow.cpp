#include "mainwindow.h"
#include "glwidget.h"
#include "debugwindow.h"
#include "meshloader.h"
#include "renderview.h"

#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QToolButton>
#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QApplication>
#include <QMessageBox>
#include <QHBoxLayout>

#if defined(GL3D_HAS_VULKAN)
#include <QVulkanInstance>
#include "vulkanwindow.h"
#endif

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    resize(1100, 720);
    setWindowTitle("gl3d 模型查看器");

    m_viewContainer = new QWidget(this);
    m_viewLayout = new QHBoxLayout(m_viewContainer);
    m_viewLayout->setContentsMargins(0, 0, 0, 0);
    m_viewLayout->setSpacing(0);
    m_glWidget = new GLWidget;
    m_glContainer = QWidget::createWindowContainer(m_glWidget, m_viewContainer);
    m_glContainer->setFocusPolicy(Qt::StrongFocus);
    m_debugWindow = new DebugWindow(m_glWidget, m_viewContainer);
    m_viewLayout->addWidget(m_debugWindow, 0);
    m_viewLayout->addWidget(m_glContainer, 1);
    setCentralWidget(m_viewContainer);

    // 工具栏 打开/复位/主题/投影/操作器
    m_toolBar = addToolBar("主工具栏");
    m_toolBar->setMovable(false);
    QAction* actOpen = m_toolBar->addAction("打开模型");
    QAction* actReset = m_toolBar->addAction("复位视角");
    m_actDebug = m_toolBar->addAction("调试器");
    m_themeBtn = new QPushButton("日间模式", this);
    m_toolBar->addSeparator();
    m_actOpenGL = new QAction("OpenGL", this);
    m_actVulkan = new QAction("Vulkan", this);
    m_actOpenGL->setCheckable(true);
    m_actVulkan->setCheckable(true);
    m_actOpenGL->setChecked(true);
    m_backendGroup = new QActionGroup(this);
    m_backendGroup->setExclusive(true);
    m_backendGroup->addAction(m_actOpenGL);
    m_backendGroup->addAction(m_actVulkan);
    m_backendButton = new QToolButton(this);
    m_backendButton->setText("后端: OpenGL");
    m_backendButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(m_backendButton, &QToolButton::clicked, this, [this]() {
        if (m_modelOpen)
            return;
        if (m_vulkanBackend)
            m_actOpenGL->setChecked(true);
        else
            m_actVulkan->setChecked(true);
    });
    m_toolBar->addWidget(m_backendButton);
#if !defined(GL3D_HAS_VULKAN)
    m_actVulkan->setEnabled(false);
    m_actVulkan->setToolTip("当前构建未找到 Vulkan");
#endif
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
    connect(m_actWire, &QAction::toggled, this, [this](bool enabled) { activeView()->setWireframe(enabled); });
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
        activeView()->setOrtho(on);
        m_actProj->setText(on ? "投影:正交" : "投影:透视");
        });
    connect(m_actMove, &QAction::toggled, this, [this](bool on) { onTransformToggled(m_actMove, on); });
    connect(m_actRot, &QAction::toggled, this, [this](bool on) { onTransformToggled(m_actRot, on); });
    connect(m_actScale, &QAction::toggled, this, [this](bool on) { onTransformToggled(m_actScale, on); });
    connect(m_actDelete, &QAction::triggered, this, [this]() {
#if defined(GL3D_HAS_VULKAN)
        if (m_vulkanBackend) {
            if (m_vulkanWindow)
                m_vulkanWindow->clearModel();
            m_modelOpen = false;
            m_modelInfo->setText("未加载模型");
            m_actDelete->setVisible(false);
            updateBackendUi();
        } else {
#else
        {
#endif
            m_glWidget->deleteModel();
        }
    });
    connect(m_actUndo, &QAction::triggered, this, [this]() { activeView()->undo(); });
    connect(m_actRedo, &QAction::triggered, this, [this]() { activeView()->redo(); });
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
    m_backendLabel = new QLabel("后端: OpenGL", this);
    statusBar()->addPermanentWidget(m_backendLabel);

    connect(actOpen, &QAction::triggered, this, [this]() {
        QString path = QFileDialog::getOpenFileName(
                           this, "选择模型", QString(), "模型文件 (*.stl *.obj *.glb);;所有文件 (*)");
        if (!path.isEmpty()) openModel(path);
        });
    connect(actReset, &QAction::triggered, this, [this]() { activeView()->resetView(); });
    m_actDebug->setCheckable(true);
    connect(m_actDebug, &QAction::toggled, m_debugWindow, &QWidget::setVisible);
    connect(m_themeBtn, &QPushButton::clicked, this, [this]() {
        applyTheme(!m_night);
        });
    connect(m_actOpenGL, &QAction::toggled, this, [this](bool on) {
        if (on) setBackend(false);
        });
    connect(m_actVulkan, &QAction::toggled, this, [this](bool on) {
        if (on) setBackend(true);
        });

    connect(m_glWidget, &GLWidget::fpsUpdated, this, [this](int fps) {
        if (m_vulkanBackend) return;
        m_fpsLabel->setText(QString("FPS %1").arg(fps));
        });
    connect(m_glWidget, &GLWidget::modelLoaded, this, [this](const QString& info) {
        m_modelInfo->setText(info);
        m_modelOpen = true;
        updateBackendUi();
        });
    connect(m_glWidget, &GLWidget::loadFailed, this, [this](const QString& err) {
        m_modelOpen = false;
        updateBackendUi();
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
    connect(m_glWidget, &GLWidget::modelCleared, this, [this]() {
        m_modelOpen = false;
        updateBackendUi();
        });
    connect(m_glWidget, &GLWidget::loadStarted, this, [this]() {
        m_modelOpen = true;
        updateBackendUi();
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
    if (!m_vulkanBackend) {
        m_glWidget->loadModel(path);
        return;
    }

#if defined(GL3D_HAS_VULKAN)
    QString error;
    if (!m_vulkanWindow->loadModel(path, &error)) {
        QMessageBox::warning(this, "加载失败", error);
        return;
    }
    m_modelOpen = true;
    m_modelInfo->setText(QString("Vulkan 后端：%1  顶点%2 三角形%3")
        .arg(QFileInfo(path).fileName()).arg(m_vulkanWindow->scene().mesh().vertexCount()).arg(m_vulkanWindow->scene().mesh().triangleCount()));
    updateBackendUi();
#else
    Q_UNUSED(path);
#endif
    }

MainWindow::~MainWindow() {
#if defined(GL3D_HAS_VULKAN)
    if (m_vulkanContainer) {
        m_debugWindow->setRenderView(m_glWidget);
        delete m_vulkanContainer;
        m_vulkanContainer = nullptr;
        m_vulkanWindow = nullptr;
    }
    delete m_vulkanInstance;
#endif
}

RenderView* MainWindow::activeView() const {
#if defined(GL3D_HAS_VULKAN)
    if (m_vulkanBackend && m_vulkanWindow) return m_vulkanWindow;
#endif
    return m_glWidget;
}

void MainWindow::setBackend(bool vulkan) {
    if (m_modelOpen || m_vulkanBackend == vulkan)
        return;

#if defined(GL3D_HAS_VULKAN)
    if (vulkan) {
        m_vulkanInstance = new QVulkanInstance;
        m_vulkanInstance->setLayers({"VK_LAYER_KHRONOS_validation"});
        if (!m_vulkanInstance->create()) {
            delete m_vulkanInstance;
            m_vulkanInstance = nullptr;
            m_actOpenGL->setChecked(true);
            QMessageBox::warning(this, "Vulkan不可用", "Vulkan Instance 创建失败，请检查驱动或验证层。");
            return;
        }
        m_vulkanWindow = new VulkanWindow(m_vulkanInstance);
        if (!m_vulkanWindow->isReady()) {
            delete m_vulkanWindow;
            m_vulkanWindow = nullptr;
            delete m_vulkanInstance;
            m_vulkanInstance = nullptr;
            m_actOpenGL->setChecked(true);
            QMessageBox::warning(this, "Vulkan不可用", "QVulkanWindow 初始化失败。");
            return;
        }
        m_vulkanWindow->setOrtho(m_actProj->isChecked());
        m_vulkanWindow->setWireframe(m_actWire->isChecked());
        m_vulkanWindow->setClearColor(m_night ? 0.13f : 0.85f,
                                      m_night ? 0.13f : 0.86f,
                                      m_night ? 0.16f : 0.88f);
        connect(m_vulkanWindow, &VulkanWindow::fpsUpdated, this, [this](int fps) {
            if (m_vulkanBackend) m_fpsLabel->setText(QString("FPS %1").arg(fps));
        });
        connect(m_vulkanWindow, &VulkanWindow::historyChanged, this, [this](bool undo, bool redo) {
            m_actUndo->setEnabled(undo);
            m_actRedo->setEnabled(redo);
        });
        connect(m_vulkanWindow, &VulkanWindow::selectionChanged, this, [this](bool selected) {
            m_actMove->setVisible(selected);
            m_actRot->setVisible(selected);
            m_actScale->setVisible(selected);
            m_actDelete->setVisible(selected || m_modelOpen);
            if (!selected && m_curTransform) m_curTransform->setChecked(false);
        });
        connect(m_vulkanWindow, &VulkanWindow::contextMenuRequested, this, [this](const QPoint& position) {
            m_contextMenu->exec(position);
        });
        m_vulkanContainer = QWidget::createWindowContainer(m_vulkanWindow, m_viewContainer);
        m_vulkanContainer->setFocusPolicy(Qt::StrongFocus);
        m_viewLayout->addWidget(m_vulkanContainer, 1);
        m_glContainer->hide();
        m_vulkanBackend = true;
        m_debugWindow->setRenderView(m_vulkanWindow);
    } else {
        m_debugWindow->setRenderView(m_glWidget);
        if (m_vulkanContainer) {
            delete m_vulkanContainer;
            m_vulkanContainer = nullptr;
            m_vulkanWindow = nullptr;
        }
        if (m_vulkanInstance) {
            delete m_vulkanInstance;
            m_vulkanInstance = nullptr;
        }
        m_glContainer->show();
        m_vulkanBackend = false;
    }
    m_fpsLabel->clear();
#else
    Q_UNUSED(vulkan);
#endif
    updateBackendUi();
}

void MainWindow::updateBackendUi() {
    const bool gl = !m_vulkanBackend;
    if (m_backendLabel)
        m_backendLabel->setText(gl ? "后端: OpenGL" : "后端: Vulkan");
    if (m_backendButton)
        m_backendButton->setText(gl ? "后端: OpenGL" : "后端: Vulkan");
    if (m_actOpenGL) m_actOpenGL->setEnabled(!m_modelOpen);
    if (m_actVulkan) m_actVulkan->setEnabled(!m_modelOpen &&
#if defined(GL3D_HAS_VULKAN)
        true
#else
        false
#endif
    );
    if (m_actDebug) m_actDebug->setEnabled(true);
    if (!gl && m_actDelete)
        m_actDelete->setVisible(m_modelOpen);
    if (m_debugWindow) m_debugWindow->setVisible(m_actDebug && m_actDebug->isChecked());
    for (QAction* action : {m_actProj, m_actWire, m_actUndo, m_actRedo, m_actMove, m_actRot, m_actScale})
        if (action) action->setEnabled(true);
    bool canUndo = m_glWidget->canUndo(), canRedo = m_glWidget->canRedo();
#if defined(GL3D_HAS_VULKAN)
    if (!gl && m_vulkanWindow) {
        canUndo = m_vulkanWindow->canUndo();
        canRedo = m_vulkanWindow->canRedo();
    }
#endif
    m_actUndo->setEnabled(canUndo);
    m_actRedo->setEnabled(canRedo);
}

// 操作器按钮互斥 选中新按钮时取消另两个勾选 再点当前按钮则取消
void MainWindow::onTransformToggled(QAction* act, bool checked) {
    if (checked) {
        m_curTransform = act;
        for (QAction* other : { m_actMove, m_actRot, m_actScale }) {
            if (other != act) other->setChecked(false);
            }
        int mode = (act == m_actMove) ? 1 : (act == m_actRot) ? 2 : 3;
        activeView()->setTransformMode(mode);
        }
    else if (m_curTransform == act) {
        m_curTransform = nullptr;
        activeView()->setTransformMode(0);
        }
    }

// 读取exe同目录qss 应用后联动GL底色
void MainWindow::applyTheme(bool night) {
    m_night = night;
    QString file = QApplication::applicationDirPath() + (night ? "/theme_night.qss" : "/theme_day.qss");
    QFile f(file);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        qApp->setStyleSheet(QString::fromUtf8(f.readAll()));

    const float red = night ? 0.13f : 0.85f;
    const float green = night ? 0.13f : 0.86f;
    const float blue = night ? 0.16f : 0.88f;
    m_glWidget->setClearColor(red, green, blue);
#if defined(GL3D_HAS_VULKAN)
    if (m_vulkanWindow) m_vulkanWindow->setClearColor(red, green, blue);
#endif
    if (m_debugWindow) m_debugWindow->setNightMode(night);
    m_themeBtn->setText(night ? "日间模式" : "夜间模式");
    }

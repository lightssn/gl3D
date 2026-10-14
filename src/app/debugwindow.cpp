#include "debugwindow.h"
#include "glwidget.h"
#include "renderview.h"
#include "processmemory.h"
#include <QElapsedTimer>
#include <QAbstractScrollArea>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGuiApplication>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QPolygonF>
#include <QSignalBlocker>
#include <QScreen>
#include <QSpinBox>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

class FrameGraph : public QWidget {
public:
    explicit FrameGraph(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(70); }
    void setSamples(const QVector<float>& values) { m_values = values; update(); }
    void setNightMode(bool night) { m_night = night; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), m_night ? QColor(10, 14, 20) : QColor(238, 241, 244));
        const int y = height() - static_cast<int>(16.67f / 50.0f * height()); p.setPen(m_night ? QColor(90, 105, 125) : QColor(170, 180, 190)); p.drawLine(0, y, width(), y);
        if (m_values.size() < 2) return;
        QPolygonF curve;
        for (int i = 0; i < m_values.size(); ++i) curve.append(QPointF(static_cast<qreal>(i) / (m_values.size() - 1) * width(), height() - std::min(m_values[i], 50.0f) / 50.0f * height()));
        p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(QColor(86, 220, 159), 1.3)); p.drawPolyline(curve);
    }
private: QVector<float> m_values; bool m_night = true;
};

static QString bytesText(qint64 bytes) {
    if (bytes < 1024) return QString("%1B").arg(bytes);
    if (bytes < 1048576) return QString("%1K").arg(bytes / 1024.0, 0, 'f', 2);
    return QString("%1M").arg(bytes / 1048576.0, 0, 'f', 2);
}

static QCheckBox* option(QGridLayout* layout, const QString& text, int row, int column, bool checked) {
    auto* box = new QCheckBox(text, layout->parentWidget()); box->setChecked(checked); layout->addWidget(box, row, column); return box;
}

DebugWindow::DebugWindow(RenderView* view, QWidget* parent) : QWidget(parent), m_renderView(view) {
    setObjectName("debugSidebar"); setMinimumWidth(350); setMaximumWidth(430); setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("#debugSidebar { background: #171c25; border-right: 1px solid #394454; } QLabel, QCheckBox { color: #e6edf3; } QGroupBox { border: 1px solid #394454; border-radius: 4px; margin-top: 7px; padding-top: 6px; color: #e6edf3; } QGroupBox::title { subcontrol-origin: margin; left: 7px; } QTableWidget { background: #0d1117; color: #e6edf3; gridline-color: #303946; } QHeaderView::section { background: #2a3240; color: #e6edf3; border: 0; padding: 3px; } QComboBox { background: #2a3240; color: #e6edf3; padding: 2px; }");
    auto* root = new QVBoxLayout(this); root->setContentsMargins(8, 6, 8, 6); root->setSpacing(5);
    auto* content = new QWidget(this); auto* layout = new QVBoxLayout(content); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(5);

    auto* renderBox = new QGroupBox("渲染管线", content); auto* renderLayout = new QGridLayout(renderBox); renderLayout->setContentsMargins(6, 8, 6, 5);
    auto* msaa = option(renderLayout, "抗锯齿 MSAA", 0, 0, true); auto* mipmap = option(renderLayout, "纹理 Mipmap", 0, 1, false); auto* depth = option(renderLayout, "深度测试", 1, 0, true); auto* culling = option(renderLayout, "背面剔除", 1, 1, false); auto* pbr = option(renderLayout, "PBR 光照", 3, 0, false); auto* normalMap = option(renderLayout, "法线贴图", 3, 1, true); auto* fixedFps = option(renderLayout, "固定帧率", 4, 0, false); auto* fpsSpin = new QSpinBox(renderBox);
    m_msaa = msaa; m_mipmap = mipmap; m_depth = depth; m_culling = culling;
    m_pbr = pbr; m_normalMap = normalMap; m_fixedFps = fixedFps; m_fpsSpin = fpsSpin;
    m_normalLighting = option(renderLayout, "法线光照", 5, 0, view->scene().options().normalLighting);
    m_normalLighting->setToolTip("关闭后仅保留环境光、材质和纹理，跳过法线相关的漫反射、高光和法线贴图；法线调试视图与描边不变。");
    connect(m_normalLighting, &QCheckBox::toggled, this, [this](bool enabled) { m_renderView->setNormalLighting(enabled); });
    QScreen* screen = QGuiApplication::primaryScreen();
    const int refreshRate = screen && screen->refreshRate() > 0.0 ? qRound(screen->refreshRate()) : 60;
    fpsSpin->setRange(0, refreshRate); fpsSpin->setValue(refreshRate); fpsSpin->setSuffix(" FPS"); fpsSpin->setToolTip("0表示不限制定时器频率"); renderLayout->addWidget(fpsSpin, 4, 1);
    connect(msaa, &QCheckBox::toggled, this, [this](bool enabled) { m_renderView->setAntialiasing(enabled); });
    connect(mipmap, &QCheckBox::toggled, this, [this](bool enabled) { m_renderView->setMipmaps(enabled); });
    connect(depth, &QCheckBox::toggled, this, [this](bool enabled) { m_renderView->setDepthTest(enabled); });
    connect(culling, &QCheckBox::toggled, this, [this](bool enabled) { m_renderView->setFaceCulling(enabled); });
    connect(pbr, &QCheckBox::toggled, this, [this](bool enabled) { m_renderView->setPbr(enabled); });
    connect(normalMap, &QCheckBox::toggled, this, [this](bool enabled) { m_renderView->setNormalMap(enabled); });
    connect(fixedFps, &QCheckBox::toggled, this, [this](bool enabled) { m_renderView->setFixedFpsEnabled(enabled); });
    connect(fpsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int fps) { m_renderView->setTargetFps(fps); });
    layout->addWidget(renderBox);
    m_outlineWidth = new QDoubleSpinBox(renderBox); m_outlineWidth->setRange(0.0, 1000.0); m_outlineWidth->setDecimals(2); m_outlineWidth->setSingleStep(0.5); m_outlineWidth->setSuffix(" px"); m_outlineWidth->setValue(view->outlineWidth());
    m_outlineWidth->setToolTip("屏幕物理像素；缩放模型或视野不改变描边宽度");
    renderLayout->addWidget(new QLabel("描边宽度（像素）", renderBox), 2, 0); renderLayout->addWidget(m_outlineWidth, 2, 1);
    connect(m_outlineWidth, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double width) { m_renderView->setOutlineWidth(static_cast<float>(width)); });

    auto* viewBox = new QGroupBox("调试视图", content); auto* viewLayout = new QGridLayout(viewBox); auto* debugView = new QComboBox(viewBox); debugView->addItems({"光照结果", "法线", "UV"}); viewLayout->addWidget(new QLabel("着色模式", viewBox), 0, 0); viewLayout->addWidget(debugView, 0, 1); connect(debugView, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this](int mode) { m_renderView->setDebugView(mode); }); layout->addWidget(viewBox);
    m_debugView = debugView;
    m_upAxis = new QComboBox(viewBox);
    m_upAxis->addItems({"Y", "Z"});
    viewLayout->addWidget(new QLabel(QStringLiteral("\u5411\u4e0a\u8f74"), viewBox), 1, 0);
    viewLayout->addWidget(m_upAxis, 1, 1);
    connect(m_upAxis, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
        [this](int index) { m_renderView->setUpAxis(index == 1 ? UpAxis::Z : UpAxis::Y); });
    m_cameraLabel = new QLabel(content); layout->addWidget(m_cameraLabel);

    auto* levelsBox = m_levelsBox = new QGroupBox(QStringLiteral("\u91d1\u5b57\u5854\u5c42\u7ea7\uff08\u5df2\u53d1\u73b0\uff09"), content);
    m_osgbBox=new QGroupBox("OSGB",content);
    auto* osgbLayout=new QHBoxLayout(m_osgbBox);
    m_onDemand=new QCheckBox(QStringLiteral("\u6309\u9700\u6a21\u5f0f"),m_osgbBox);
    m_onDemand->setChecked(view->scene().options().osgbOnDemand);
    m_onDemand->setToolTip(QStringLiteral("\u89c6\u91ce\u5185\u74e6\u7247\u52a0\u8f7d\u548c\u4e0a\u4f20\u5b8c\u6210\u540e\u505c\u6b62\u91cd\u7ed8\uff0c\u7528\u6237\u64cd\u4f5c\u540e\u6062\u590d\u3002"));
    osgbLayout->addWidget(m_onDemand); layout->addWidget(m_osgbBox);
    m_tileBounds=new QCheckBox(QStringLiteral("\u663e\u793a\u74e6\u7247\u5916\u6846"),m_osgbBox);
    osgbLayout->addWidget(m_tileBounds);
    connect(m_tileBounds,&QCheckBox::toggled,this,[this](bool enabled) { m_renderView->setTileBounds(enabled); });
    m_osgbBox->setVisible(view->scene().tiled());
    connect(m_onDemand,&QCheckBox::toggled,this,[this](bool enabled) {
        m_renderView->scene().options().osgbOnDemand=enabled;
        m_renderView->requestRedraw();
    });
    auto* levelsLayout = new QVBoxLayout(levelsBox);
    levelsBox->setToolTip(QStringLiteral("\u7ec6\u5c42\u968f\u5206\u9875\u52a0\u8f7d\u9010\u6b65\u53d1\u73b0\u3002L0 \u6700\u7c97\uff1b\u8ddd\u79bb\u4f7f\u7528\u6a21\u578b\u5355\u4f4d\uff0c\u50cf\u7d20 LOD \u4f7f\u7528 px\u3002\n\u53d6\u6d88\u52fe\u9009\u9690\u85cf\u8be5\u5c42\uff0c\u4ecd\u53ef\u8bbf\u95ee\u66f4\u7ec6\u5c42\u3002"));
    levelsBox->setVisible(view->scene().tiled());
    m_levelTint = new QCheckBox(QStringLiteral("\u6309\u5c42\u7ea7\u53e0\u52a0\u534a\u900f\u660e\u989c\u8272"),levelsBox);
    connect(m_levelTint,&QCheckBox::toggled,this,[this](bool enabled) { m_renderView->scene().options().tileLevelTint=enabled; m_renderView->requestRedraw(); });
    levelsLayout->addWidget(m_levelTint);
    m_levels = new QTableWidget(0,3,levelsBox);
    m_levels->setHorizontalHeaderLabels({QStringLiteral("\u5c42\u7ea7"),QStringLiteral("\u6700\u8fd1"),QStringLiteral("\u6700\u8fdc")});
    m_levels->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_levels->verticalHeader()->hide(); m_levels->setMinimumHeight(110); m_levels->setMaximumHeight(210);
    m_levels->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_levels->setToolTip(levelsBox->toolTip());
    levelsLayout->addWidget(m_levels);
    auto* resetLevels = new QPushButton(QStringLiteral("\u6062\u590d\u539f\u59cb LOD \u8bbe\u7f6e"),levelsBox);
    connect(resetLevels,&QPushButton::clicked,this,[this] { m_renderView->scene().resetTileLevels(); m_renderView->requestRedraw(); refreshTileLevels(); });
    levelsLayout->addWidget(resetLevels); layout->addWidget(levelsBox);

    auto* cacheBox=m_cacheBox=new QGroupBox(QStringLiteral("\u74e6\u7247\u7f13\u5b58"),content);
    cacheBox->setVisible(view->scene().tiled());
    auto* cacheLayout=new QGridLayout(cacheBox);
    m_cacheBudget=new QSpinBox(cacheBox); m_cacheBudget->setRange(1,65536); m_cacheBudget->setValue(1024); m_cacheBudget->setSuffix(" MiB");
    cacheLayout->addWidget(new QLabel(QStringLiteral("CPU + GPU \u6570\u636e\u9884\u7b97"),cacheBox),0,0); cacheLayout->addWidget(m_cacheBudget,0,1);
    m_cacheEviction=new QCheckBox(QStringLiteral("\u8d85\u9884\u7b97\u65f6\u4f18\u5148\u56de\u6536\u6df1\u5c42\u95f2\u7f6e\u9875"),cacheBox);
    m_cacheEviction->setToolTip(QStringLiteral("\u9ed8\u8ba4\u4e0d\u56de\u6536\uff1b\u542f\u7528\u540e\u4fdd\u62a4\u5f53\u524d\u9700\u8981\u3001\u52a0\u8f7d\u4e2d\u548c\u9009\u4e2d\u7684\u9875\u3002\u9884\u7b97\u4e0d\u662f\u8fdb\u7a0b\u5185\u5b58\u4e0a\u9650\u3002"));
    cacheLayout->addWidget(m_cacheEviction,1,0,1,2);
    auto policy=[this] { m_renderView->scene().setTileCachePolicy(qint64(m_cacheBudget->value())*1024*1024,m_cacheEviction->isChecked()); m_renderView->requestRedraw(); };
    connect(m_cacheEviction,&QCheckBox::toggled,this,policy); connect(m_cacheBudget,qOverload<int>(&QSpinBox::valueChanged),this,policy);
    m_memoryLabel=new QLabel(cacheBox); m_memoryLabel->setWordWrap(true); cacheLayout->addWidget(m_memoryLabel,2,0,1,2);
    layout->addWidget(cacheBox);

    auto* resourceBox = new QGroupBox("GPU 对象占用", content); auto* resourceLayout = new QVBoxLayout(resourceBox); resourceLayout->setContentsMargins(5, 8, 5, 5);
    m_resources = new QTableWidget(4, 5, resourceBox); m_resources->setHorizontalHeaderLabels({"VAO", "VBO", "EBO", "纹理", "FBO"}); m_resources->setVerticalHeaderLabels({"数量", "内存", "显存", "总计"}); m_resources->setSpan(3, 0, 1, 5); m_resources->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); m_resources->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed); m_resources->setEditTriggers(QAbstractItemView::NoEditTriggers); m_resources->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_resources->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_resources->setFocusPolicy(Qt::NoFocus); m_resources->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    for (int row = 0; row < m_resources->rowCount(); ++row) m_resources->setRowHeight(row, 25);
    const int resourceTableHeight = m_resources->horizontalHeader()->sizeHint().height() + 4 * 25 + 2;
    m_resources->setMinimumHeight(resourceTableHeight); resourceBox->setMinimumHeight(resourceTableHeight + 28); resourceLayout->addWidget(m_resources); layout->addWidget(resourceBox);

    m_timingLabel = new QLabel(content); layout->addWidget(m_timingLabel); m_graph = new FrameGraph(content); layout->addWidget(m_graph);
    auto* subMeshBox = new QGroupBox("子网格", content); auto* subMeshLayout = new QVBoxLayout(subMeshBox); subMeshLayout->setContentsMargins(5, 8, 5, 5);
    m_subMeshes = new QListWidget(subMeshBox); m_subMeshes->setSelectionMode(QAbstractItemView::SingleSelection); m_subMeshes->setMinimumHeight(0); m_subMeshes->setMaximumHeight(180); m_subMeshes->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_subMeshes->setTextElideMode(Qt::ElideRight); m_subMeshes->setStyleSheet("QListWidget { background: #0d1117; color: #e6edf3; } QListWidget::item:selected { background: #246a9e; color: #ffffff; } QListWidget::item:hover { background: #1d3f5a; }");
    connect(m_subMeshes, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        m_renderView->setSubMeshVisible(m_subMeshes->row(item), item->checkState() == Qt::Checked);
        });
    connect(m_subMeshes, &QListWidget::currentRowChanged, this, [this](int row) {
        m_renderView->selectSubMesh(row);
        });
    subMeshLayout->addWidget(m_subMeshes);
    layout->addWidget(subMeshBox);
    layout->addStretch(1); m_driverLabel = new QLabel(content); m_driverLabel->setWordWrap(true); layout->addWidget(m_driverLabel); auto* scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(content); root->addWidget(scroll, 1);
    m_timer = new QTimer(this); connect(m_timer, &QTimer::timeout, this, &DebugWindow::refresh); m_timer->start(250); hide();
}

void DebugWindow::setRenderView(RenderView* view) {
    view->setTileBounds(m_tileBounds->isChecked());
    m_renderView = view;
    m_subMeshCount = -1;
    m_resources->setHorizontalHeaderLabels(view->backend() == RenderBackend::Vulkan
        ? QStringList{"VAO", "缓冲", "EBO", "纹理", "FBO"}
        : QStringList{"VAO", "VBO", "EBO", "纹理", "FBO"});
    {
        QSignalBlocker blocker(m_outlineWidth);
        m_outlineWidth->setValue(view->outlineWidth());
    }
    m_msaa->setEnabled(view->supportsAntialiasing());
    if (!view->supportsAntialiasing()) {
        QSignalBlocker blocker(m_msaa);
        m_msaa->setChecked(false);
    }
    view->setAntialiasing(m_msaa->isChecked());
    view->setMipmaps(m_mipmap->isChecked());
    view->setDepthTest(m_depth->isChecked());
    view->setFaceCulling(m_culling->isChecked());
    view->setPbr(m_pbr->isChecked());
    view->setNormalMap(m_normalMap->isChecked());
    view->setNormalLighting(m_normalLighting->isChecked());
    view->setFixedFpsEnabled(m_fixedFps->isChecked());
    view->setTargetFps(m_fpsSpin->value());
    view->setDebugView(m_debugView->currentIndex());
    { QSignalBlocker blocker(m_upAxis); m_upAxis->setCurrentIndex(int(view->scene().cameraController().camera().upAxis())); }
    refreshSubMeshes();
    refreshTileLevels();
    m_pbr->setEnabled(m_renderView->supportsPbr());
    m_normalMap->setEnabled(m_renderView->supportsNormalMap());
    m_pbr->setToolTip(m_renderView->supportsPbr()?QString():QStringLiteral("OSGB 基础材质未提供 PBR 数据"));
    m_normalMap->setToolTip(m_renderView->supportsNormalMap()?QString():QStringLiteral("OSGB 导入未映射法线贴图"));
    if(!m_renderView->supportsPbr()) { QSignalBlocker blocker(m_pbr); m_pbr->setChecked(false); }
    if(!m_renderView->supportsNormalMap()) { QSignalBlocker blocker(m_normalMap); m_normalMap->setChecked(false); }
    refresh();
}

void DebugWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshSubMeshes();
    refreshTileLevels();
    m_pbr->setEnabled(m_renderView->supportsPbr());
    m_normalMap->setEnabled(m_renderView->supportsNormalMap());
    m_pbr->setToolTip(m_renderView->supportsPbr()?QString():QStringLiteral("OSGB 基础材质未提供 PBR 数据"));
    m_normalMap->setToolTip(m_renderView->supportsNormalMap()?QString():QStringLiteral("OSGB 导入未映射法线贴图"));
    if(!m_renderView->supportsPbr()) { QSignalBlocker blocker(m_pbr); m_pbr->setChecked(false); }
    if(!m_renderView->supportsNormalMap()) { QSignalBlocker blocker(m_normalMap); m_normalMap->setChecked(false); }
    refresh();
}

void DebugWindow::setNightMode(bool night) {
    if (night) {
        setStyleSheet("#debugSidebar { background: #171c25; border-right: 1px solid #394454; } QLabel, QCheckBox { color: #e6edf3; } QGroupBox { border: 1px solid #394454; border-radius: 4px; margin-top: 7px; padding-top: 6px; color: #e6edf3; } QGroupBox::title { subcontrol-origin: margin; left: 7px; } QTableWidget { background: #0d1117; color: #e6edf3; gridline-color: #303946; } QHeaderView::section { background: #2a3240; color: #e6edf3; border: 0; padding: 3px; } QComboBox { background: #2a3240; color: #e6edf3; padding: 2px; }");
    } else {
        setStyleSheet("#debugSidebar { background: #f1f3f5; border-right: 1px solid #b8c1cc; } QLabel, QCheckBox { color: #20252b; } QGroupBox { border: 1px solid #aeb8c4; border-radius: 4px; margin-top: 7px; padding-top: 6px; color: #20252b; } QGroupBox::title { subcontrol-origin: margin; left: 7px; } QTableWidget { background: #ffffff; color: #20252b; gridline-color: #c8d0d8; } QHeaderView::section { background: #dce2e8; color: #20252b; border: 0; padding: 3px; } QComboBox { background: #ffffff; color: #20252b; padding: 2px; }");
    }
    const QString indicatorStyle = night
        ? QStringLiteral(" QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #8c9aaa; background: #202733; border-radius: 2px; } QCheckBox::indicator:checked { background: #48a6e8; border-color: #72c4f5; image: none; }")
        : QStringLiteral(" QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #687583; background: #ffffff; border-radius: 2px; } QCheckBox::indicator:checked { background: #2788c7; border-color: #17699e; image: none; }");
    setStyleSheet(styleSheet() + indicatorStyle);
    if (m_subMeshes) {
        m_subMeshes->setStyleSheet(night
            ? "QListWidget { background: #0d1117; color: #e6edf3; } QListWidget::item:selected { background: #246a9e; color: #ffffff; } QListWidget::item:hover { background: #1d3f5a; }"
            : "QListWidget { background: #ffffff; color: #20252b; } QListWidget::item:selected { background: #3b82b6; color: #ffffff; } QListWidget::item:hover { background: #d9eaf7; }");
    }
    if (m_graph) m_graph->setNightMode(night);
}

void DebugWindow::refresh() {
    if (!isVisible()) return;
    m_osgbBox->setVisible(m_renderView->scene().tiled());
    m_levelsBox->setVisible(m_renderView->scene().tiled());
    m_cacheBox->setVisible(m_renderView->scene().tiled());
    { QSignalBlocker blocker(m_onDemand); m_onDemand->setChecked(m_renderView->scene().options().osgbOnDemand); }
    m_tileBounds->setEnabled(m_renderView->scene().tiled());
    const DebugSnapshot s = m_renderView->debugSnapshot();
    m_cacheEviction->setEnabled(m_renderView->scene().tiled()); m_cacheBudget->setEnabled(m_renderView->scene().tiled());
    { QSignalBlocker block(m_cacheEviction); m_cacheEviction->setChecked(m_renderView->scene().tileBudgetEviction()); }
    if(!m_cacheBudget->hasFocus()) { QSignalBlocker block(m_cacheBudget); m_cacheBudget->setValue(int(m_renderView->scene().tileBudgetBytes()/(1024*1024))); }
    static QElapsedTimer memoryTimer;
    static ProcessMemory process;
    if(!memoryTimer.isValid() || memoryTimer.elapsed()>=1000) { process=processMemory(); memoryTimer.restart(); }
    QString memoryText;
    if(process.valid) {
        memoryText=QStringLiteral("\u8fdb\u7a0b\u5de5\u4f5c\u96c6 %1\n\u79c1\u6709\u5de5\u4f5c\u96c6 %2 / \u79c1\u6709\u63d0\u4ea4 %3")
            .arg(bytesText(process.workingSet),process.privateWorkingSetValid?bytesText(process.privateWorkingSet):QStringLiteral("N/A"),bytesText(process.privateCommit));
    }
    if(m_renderView->scene().tiled()) {
        const auto memory=m_renderView->scene().tileStats();
        memoryText+=QStringLiteral("\nCPU \u74e6\u7247\u7f13\u5b58\u4f30\u7b97 %1\nGPU \u74e6\u7247\u6570\u636e\u4f30\u7b97 %2\n\u540e\u53f0\u8f6c\u6362\u5df2\u77e5\u6570\u636e\u4f30\u7b97 %3\uff08\u8bfb\u53d6\u4e2d %4\uff09\n\u7f13\u5b58\u9884\u7b97\u8ba1\u8d39 %5 / %6")
            .arg(bytesText(memory.cpuBytes),bytesText(memory.gpuBytes),bytesText(memory.backgroundBytes)).arg(memory.readingJobs)
            .arg(bytesText(memory.bytes),bytesText(m_renderView->scene().tileBudgetBytes()));
    }
    m_memoryLabel->setText(memoryText);
    m_memoryLabel->setToolTip(QStringLiteral("\u79c1\u6709\u5de5\u4f5c\u96c6\u5bf9\u7167\u4efb\u52a1\u7ba1\u7406\u5668\u7684\u8fdb\u7a0b\u5185\u5b58\u5217\uff0c\u6bcf\u79d2\u91c7\u6837\u3002\u8d44\u6e90\u4f30\u7b97\u4e0d\u7b49\u4e8e\u8fdb\u7a0b\u603b\u5360\u7528\uff0c\u4e0d\u80fd\u4e0e\u5de5\u4f5c\u96c6\u76f4\u63a5\u76f8\u52a0\u3002OSG \u8bfb\u53d6\u5185\u90e8\u3001\u5206\u914d\u5668\u548c\u9a71\u52a8\u5f00\u9500\u4e0d\u80fd\u7cbe\u786e\u5f52\u5c5e\u5230\u4e00\u4e2a\u74e6\u7247\u3002"));
    { QSignalBlocker blocker(m_upAxis); m_upAxis->setCurrentIndex(int(m_renderView->scene().cameraController().camera().upAxis())); }
    refreshSubMeshes();
    refreshTileLevels();
    m_pbr->setEnabled(m_renderView->supportsPbr());
    m_normalMap->setEnabled(m_renderView->supportsNormalMap());
    m_pbr->setToolTip(m_renderView->supportsPbr()?QString():QStringLiteral("OSGB 基础材质未提供 PBR 数据"));
    m_normalMap->setToolTip(m_renderView->supportsNormalMap()?QString():QStringLiteral("OSGB 导入未映射法线贴图"));
    if(!m_renderView->supportsPbr()) { QSignalBlocker blocker(m_pbr); m_pbr->setChecked(false); }
    if(!m_renderView->supportsNormalMap()) { QSignalBlocker blocker(m_normalMap); m_normalMap->setChecked(false); }
    if (!m_outlineWidth->hasFocus() && std::abs(m_outlineWidth->value() - m_renderView->outlineWidth()) > 0.0001) {
        QSignalBlocker blocker(m_outlineWidth);
        m_outlineWidth->setValue(m_renderView->outlineWidth());
    }
    m_cameraLabel->setText(QString("相机：%1  FOV：%2°  距离：%3\n眼睛 (%4, %5, %6)  目标 (%7, %8, %9)").arg(s.ortho ? "正交" : "透视").arg(s.fov, 0, 'f', 1).arg(s.distance, 0, 'f', 3).arg(s.eye.x(), 0, 'f', 2).arg(s.eye.y(), 0, 'f', 2).arg(s.eye.z(), 0, 'f', 2).arg(s.target.x(), 0, 'f', 2).arg(s.target.y(), 0, 'f', 2).arg(s.target.z(), 0, 'f', 2));
    m_timingLabel->setText(QString("FPS：%1   平均：%2 ms   1% Low：%3 FPS\nP95：%4 ms   P99：%5 ms   最慢：%6 ms\n绘制调用：%7").arg(s.fps).arg(s.averageFrameMs, 0, 'f', 2).arg(s.onePercentLowFps, 0, 'f', 1).arg(s.p95FrameMs, 0, 'f', 2).arg(s.p99FrameMs, 0, 'f', 2).arg(s.lowFrameMs, 0, 'f', 2).arg(s.drawCalls));
    if(m_renderView->scene().tiled()) {
        const auto stats=m_renderView->scene().tileStats();
        m_timingLabel->setText(m_timingLabel->text()+QString("\n瓦片单元：驻留 %1 / 可见 %2\n加载 %3 / 排队 %4 / 回收 %5 / 失败 %6\n驻留估算 %7").arg(stats.resident).arg(stats.visible).arg(stats.loading).arg(stats.queued).arg(stats.evicted).arg(stats.failures).arg(bytesText(stats.bytes)));
        m_timingLabel->setToolTip(stats.lastError);
    } else m_timingLabel->setToolTip(QString());
    m_graph->setSamples(s.frameTimesMs);
    m_resources->clearContents();
    qint64 totalCpu = 0, totalGpu = 0;
    bool hasDriverManagedMemory = false;
    for (int i = 0; i < s.resources.size() && i < 5; ++i) {
        const auto& resource = s.resources[i];
        m_resources->setItem(0, i, new QTableWidgetItem(QString::number(resource.count)));
        m_resources->setItem(1, i, new QTableWidgetItem(bytesText(resource.cpuBytes)));
        const QString gpuText = resource.queryable ? bytesText(resource.bytes)
            : resource.bytes > 0 ? QStringLiteral("≥") + bytesText(resource.bytes) : QStringLiteral("驱动管理");
        auto* gpuItem = new QTableWidgetItem(gpuText);
        if (!resource.queryable) {
            gpuItem->setToolTip("仅含程序可查询的分配；Qt/驱动管理的附件或交换链不计入");
            hasDriverManagedMemory = hasDriverManagedMemory || resource.count > 0;
        }
        m_resources->setItem(2, i, gpuItem);
        totalCpu += resource.cpuBytes;
        totalGpu += resource.bytes;
    }
    auto* totalItem = new QTableWidgetItem(QString("资源CPU %1 GPU %2").arg(bytesText(totalCpu), bytesText(totalGpu)));
    if (hasDriverManagedMemory) totalItem->setToolTip("已知占用合计，不含 Qt/驱动管理的 GPU 分配");
    totalItem->setTextAlignment(Qt::AlignCenter);
    m_resources->setItem(3, 0, totalItem);
    m_driverLabel->setText(QString("显卡：%1\n%2").arg(s.renderer, s.glVersion));
    const int selected = m_renderView->selectedSubMesh();
    if (m_subMeshes->currentRow() != selected && selected < m_subMeshes->count()) {
        QSignalBlocker blocker(m_subMeshes);
        m_subMeshes->setCurrentRow(selected);
    }
}

void DebugWindow::refreshSubMeshes() {
    const int count = m_renderView->subMeshCount();
    bool changed = count != m_subMeshCount;
    if (!changed) {
        for (int i = 0; i < count; ++i)
            if (!m_subMeshes->item(i) || m_subMeshes->item(i)->text() != m_renderView->subMeshName(i)) { changed = true; break; }
    }
    if (!changed) {
        QSignalBlocker blocker(m_subMeshes);
        for (int i = 0; i < count; ++i)
        {
            auto* item = m_subMeshes->item(i);
            const bool resident = !m_renderView->scene().tiled() || m_renderView->scene().mesh().subMeshes[size_t(i)].resident;
            item->setFlags(resident ? item->flags() | Qt::ItemIsEnabled : item->flags() & ~Qt::ItemIsEnabled);
            item->setCheckState(m_renderView->subMeshVisible(i) ? Qt::Checked : Qt::Unchecked);
        }
        return;
    }
    QSignalBlocker blocker(m_subMeshes);
    m_subMeshes->clear();
    for (int i = 0; i < count; ++i) {
        auto* item = new QListWidgetItem(m_renderView->subMeshName(i), m_subMeshes);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        if (m_renderView->scene().tiled() && !m_renderView->scene().mesh().subMeshes[size_t(i)].resident)
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        item->setCheckState(m_renderView->subMeshVisible(i) ? Qt::Checked : Qt::Unchecked);
    }
    m_subMeshCount = count;
    m_subMeshes->setFixedHeight(std::min(180, count * 25 + (count > 0 ? 2 : 0)));
    const int selected = m_renderView->selectedSubMesh();
    if (selected >= 0 && selected < count)
        m_subMeshes->setCurrentRow(selected);
    }

void DebugWindow::applyTileLevel(int row,bool overrideRange) {
    auto* enabled=m_levels->cellWidget(row,0)->findChild<QCheckBox*>();
    auto* minimum=qobject_cast<QDoubleSpinBox*>(m_levels->cellWidget(row,1));
    auto* maximum=qobject_cast<QDoubleSpinBox*>(m_levels->cellWidget(row,2));
    const auto levels=m_renderView->scene().tileLevels();
    if(!enabled || !minimum || !maximum || row>=int(levels.size())) return;
    const auto& setting=levels[size_t(row)];
    // Merely focusing a field must not replace authored ranges with their summary.
    if(overrideRange && std::abs(minimum->value()-setting.minRange)<0.0051 &&
       std::abs(maximum->value()-std::min(1e30,double(setting.maxRange)))<0.0051) return;
    if(!overrideRange) {
        m_renderView->scene().setTileLevel(row,enabled->isChecked(),setting.minRange,setting.maxRange,setting.overridden);
        m_renderView->requestRedraw();
        return;
    }
    const float high=maximum->value()>=1e30 ? std::numeric_limits<float>::max() : float(maximum->value());
    if(minimum->value()>=high) { refreshTileLevels(); return; }
    m_renderView->scene().setTileLevel(row,enabled->isChecked(),float(minimum->value()),high,overrideRange || levels[size_t(row)].overridden);
    m_renderView->requestRedraw();
}

void DebugWindow::refreshTileLevels() {
    const auto levels=m_renderView->scene().tileLevels();
    m_levels->setEnabled(!levels.empty());
    m_levelTint->setEnabled(!levels.empty());
    { QSignalBlocker block(m_levelTint); m_levelTint->setChecked(m_renderView->scene().options().tileLevelTint); }
    if(m_levels->rowCount()>int(levels.size())) m_levels->setRowCount(int(levels.size()));
    while(m_levels->rowCount()<int(levels.size())) {
        const int row=m_levels->rowCount(); m_levels->insertRow(row);
        auto* cell=new QWidget(m_levels);
        auto* cellLayout=new QHBoxLayout(cell); cellLayout->setContentsMargins(4,0,4,0); cellLayout->setSpacing(6);
        auto* enabled=new QCheckBox(QString("L%1").arg(row),cell);
        auto* swatch=new QLabel(cell); swatch->setFixedSize(12,12);
        const auto color=RenderScene::tileLevelColor(row);
        const QColor fill=QColor::fromRgbF(color.x(),color.y(),color.z());
        swatch->setStyleSheet(QString("background-color: %1; border: 1px solid #687583;").arg(fill.name()));
        cellLayout->addWidget(enabled); cellLayout->addWidget(swatch); cellLayout->addStretch();
        m_levels->setCellWidget(row,0,cell);
        connect(enabled,&QCheckBox::toggled,this,[this,row] { applyTileLevel(row,false); });
        for(int column:{1,2}) {
            auto* spin=new QDoubleSpinBox(m_levels); spin->setRange(0,1e30); spin->setDecimals(2); spin->setSingleStep(100);
            spin->setKeyboardTracking(false); spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setToolTip(QStringLiteral("\u6700\u8fdc 1e30 \u8868\u793a\u65e0\u9650\uff1b\u8303\u56f4\u4e3a [\u6700\u8fd1, \u6700\u8fdc)\u3002"));
            m_levels->setCellWidget(row,column,spin);
            connect(spin,&QDoubleSpinBox::editingFinished,this,[this,row] { applyTileLevel(row,true); });
        }

    }
    for(int row=0;row<int(levels.size());++row) {
        const auto& setting=levels[size_t(row)];
        auto* enabled=m_levels->cellWidget(row,0)->findChild<QCheckBox*>();
        QSignalBlocker blocker(enabled); enabled->setChecked(setting.enabled);
        enabled->setToolTip(setting.overridden?QStringLiteral("\u81ea\u5b9a\u4e49\u8303\u56f4"):QStringLiteral("\u6587\u4ef6\u539f\u59cb\u8303\u56f4"));
        for(int column:{1,2}) {
            auto* spin=qobject_cast<QDoubleSpinBox*>(m_levels->cellWidget(row,column));
            if(!spin->hasFocus()) { QSignalBlocker block(spin); spin->setValue(std::min(1e30,double(column==1?setting.minRange:setting.maxRange))); }
        }
    }
}

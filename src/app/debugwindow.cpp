#include "debugwindow.h"
#include "glwidget.h"
#include <QAbstractScrollArea>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPolygonF>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

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

DebugWindow::DebugWindow(GLWidget* glWidget, QWidget* parent) : QWidget(parent), m_glWidget(glWidget) {
    setObjectName("debugSidebar"); setMinimumWidth(350); setMaximumWidth(430); setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("#debugSidebar { background: #171c25; border-right: 1px solid #394454; } QLabel, QCheckBox { color: #e6edf3; } QGroupBox { border: 1px solid #394454; border-radius: 4px; margin-top: 7px; padding-top: 6px; color: #e6edf3; } QGroupBox::title { subcontrol-origin: margin; left: 7px; } QTableWidget { background: #0d1117; color: #e6edf3; gridline-color: #303946; } QHeaderView::section { background: #2a3240; color: #e6edf3; border: 0; padding: 3px; } QComboBox { background: #2a3240; color: #e6edf3; padding: 2px; }");
    auto* root = new QVBoxLayout(this); root->setContentsMargins(8, 6, 8, 6); root->setSpacing(5);
    auto* content = new QWidget(this); auto* layout = new QVBoxLayout(content); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(5);

    auto* renderBox = new QGroupBox("渲染管线", content); auto* renderLayout = new QGridLayout(renderBox); renderLayout->setContentsMargins(6, 8, 6, 5);
    auto* msaa = option(renderLayout, "抗锯齿 MSAA", 0, 0, true); auto* mipmap = option(renderLayout, "纹理 Mipmap", 0, 1, false); auto* depth = option(renderLayout, "深度测试", 1, 0, true); auto* culling = option(renderLayout, "背面剔除", 1, 1, false); auto* pbr = option(renderLayout, "PBR 光照", 3, 0, false); auto* normalMap = option(renderLayout, "法线贴图", 3, 1, true);
    connect(msaa, &QCheckBox::toggled, glWidget, &GLWidget::setAntialiasing); connect(mipmap, &QCheckBox::toggled, glWidget, &GLWidget::setMipmaps); connect(depth, &QCheckBox::toggled, glWidget, &GLWidget::setDepthTest); connect(culling, &QCheckBox::toggled, glWidget, &GLWidget::setFaceCulling); connect(pbr, &QCheckBox::toggled, glWidget, &GLWidget::setPbr); connect(normalMap, &QCheckBox::toggled, glWidget, &GLWidget::setNormalMap); layout->addWidget(renderBox);
    m_outlineWidth = new QDoubleSpinBox(renderBox); m_outlineWidth->setRange(0.0, 1000.0); m_outlineWidth->setDecimals(4); m_outlineWidth->setSingleStep(0.01); m_outlineWidth->setSuffix(" 模型单位"); m_outlineWidth->setValue(glWidget->outlineWidth());
    renderLayout->addWidget(new QLabel("描边宽度（模型单位）", renderBox), 2, 0); renderLayout->addWidget(m_outlineWidth, 2, 1);
    connect(m_outlineWidth, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [glWidget](double width) { glWidget->setOutlineWidth(static_cast<float>(width)); });

    auto* viewBox = new QGroupBox("调试视图", content); auto* viewLayout = new QGridLayout(viewBox); auto* debugView = new QComboBox(viewBox); debugView->addItems({"光照结果", "法线", "UV"}); viewLayout->addWidget(new QLabel("着色模式", viewBox), 0, 0); viewLayout->addWidget(debugView, 0, 1); connect(debugView, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), glWidget, &GLWidget::setDebugView); layout->addWidget(viewBox);
    m_cameraLabel = new QLabel(content); layout->addWidget(m_cameraLabel);

    auto* resourceBox = new QGroupBox("GPU 对象占用", content); auto* resourceLayout = new QVBoxLayout(resourceBox); resourceLayout->setContentsMargins(5, 8, 5, 5);
    m_resources = new QTableWidget(4, 5, resourceBox); m_resources->setHorizontalHeaderLabels({"VAO", "VBO", "EBO", "纹理", "FBO"}); m_resources->setVerticalHeaderLabels({"数量", "内存", "显存", "总计"}); m_resources->setSpan(3, 0, 1, 5); m_resources->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); m_resources->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed); m_resources->setEditTriggers(QAbstractItemView::NoEditTriggers); m_resources->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_resources->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_resources->setFocusPolicy(Qt::NoFocus); m_resources->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    for (int row = 0; row < m_resources->rowCount(); ++row) m_resources->setRowHeight(row, 25);
    const int resourceTableHeight = m_resources->horizontalHeader()->sizeHint().height() + 4 * 25 + 2;
    m_resources->setMinimumHeight(resourceTableHeight); resourceBox->setMinimumHeight(resourceTableHeight + 28); resourceLayout->addWidget(m_resources); layout->addWidget(resourceBox);

    m_timingLabel = new QLabel(content); layout->addWidget(m_timingLabel); m_graph = new FrameGraph(content); layout->addWidget(m_graph);
    auto* subMeshBox = new QGroupBox("子网格", content); auto* subMeshLayout = new QVBoxLayout(subMeshBox); subMeshLayout->setContentsMargins(5, 8, 5, 5);
    m_subMeshes = new QListWidget(subMeshBox); m_subMeshes->setSelectionMode(QAbstractItemView::SingleSelection); m_subMeshes->setMinimumHeight(0); m_subMeshes->setMaximumHeight(180); m_subMeshes->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_subMeshes->setTextElideMode(Qt::ElideRight); m_subMeshes->setStyleSheet("QListWidget { background: #0d1117; color: #e6edf3; } QListWidget::item:selected { background: #246a9e; color: #ffffff; } QListWidget::item:hover { background: #1d3f5a; }");
    connect(m_subMeshes, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        m_glWidget->setSubMeshVisible(m_subMeshes->row(item), item->checkState() == Qt::Checked);
        });
    connect(m_subMeshes, &QListWidget::currentRowChanged, this, [this](int row) {
        m_glWidget->selectSubMesh(row);
        });
    connect(glWidget, &GLWidget::subMeshSelected, this, [this](int index) {
        QSignalBlocker blocker(m_subMeshes);
        m_subMeshes->setCurrentRow(index);
    });
    subMeshLayout->addWidget(m_subMeshes);
    layout->addWidget(subMeshBox);
    layout->addStretch(1); m_driverLabel = new QLabel(content); m_driverLabel->setWordWrap(true); layout->addWidget(m_driverLabel); root->addWidget(content, 1);
    m_timer = new QTimer(this); connect(m_timer, &QTimer::timeout, this, &DebugWindow::refresh); m_timer->start(250); hide();
    connect(glWidget, &GLWidget::modelLoaded, this, [this](const QString&) { m_outlineWidth->setValue(m_glWidget->outlineWidth()); refreshSubMeshes(); });
    connect(glWidget, &GLWidget::modelCleared, this, [this]() { refreshSubMeshes(); });
}

void DebugWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshSubMeshes();
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
    const DebugSnapshot s = m_glWidget->debugSnapshot();
    refreshSubMeshes();
    m_cameraLabel->setText(QString("相机：%1  FOV：%2°  距离：%3\n眼睛 (%4, %5, %6)  目标 (%7, %8, %9)").arg(s.ortho ? "正交" : "透视").arg(s.fov, 0, 'f', 1).arg(s.distance, 0, 'f', 3).arg(s.eye.x(), 0, 'f', 2).arg(s.eye.y(), 0, 'f', 2).arg(s.eye.z(), 0, 'f', 2).arg(s.target.x(), 0, 'f', 2).arg(s.target.y(), 0, 'f', 2).arg(s.target.z(), 0, 'f', 2));
    m_timingLabel->setText(QString("FPS：%1   平均：%2 ms   1% Low：%3 FPS\nP95：%4 ms   P99：%5 ms   最慢：%6 ms\n绘制调用：%7").arg(s.fps).arg(s.averageFrameMs, 0, 'f', 2).arg(s.onePercentLowFps, 0, 'f', 1).arg(s.p95FrameMs, 0, 'f', 2).arg(s.p99FrameMs, 0, 'f', 2).arg(s.lowFrameMs, 0, 'f', 2).arg(s.drawCalls));
    m_graph->setSamples(s.frameTimesMs); m_resources->clearContents(); qint64 totalCpu = 0, totalGpu = 0;
    for (int i = 0; i < s.resources.size() && i < 5; ++i) { const auto& r = s.resources[i]; m_resources->setItem(0, i, new QTableWidgetItem(QString::number(r.count))); m_resources->setItem(1, i, new QTableWidgetItem(bytesText(r.cpuBytes))); m_resources->setItem(2, i, new QTableWidgetItem(r.queryable ? bytesText(r.bytes) : "驱动管理")); totalCpu += r.cpuBytes; if (r.queryable) totalGpu += r.bytes; }
    auto* totalItem = new QTableWidgetItem(QString("内存 %1 显存 %2").arg(bytesText(totalCpu), bytesText(totalGpu))); totalItem->setTextAlignment(Qt::AlignCenter); m_resources->setItem(3, 0, totalItem);
    m_driverLabel->setText(QString("显卡：%1\nOpenGL：%2").arg(s.renderer, s.glVersion));
    const int selected = m_glWidget->selectedSubMesh();
    if (selected >= 0 && selected < m_subMeshes->count()) {
        QSignalBlocker blocker(m_subMeshes);
        m_subMeshes->setCurrentRow(selected);
    }
}

void DebugWindow::refreshSubMeshes() {
    const int count = m_glWidget->subMeshCount();
    bool changed = count != m_subMeshCount;
    if (!changed) {
        for (int i = 0; i < count; ++i)
            if (!m_subMeshes->item(i) || m_subMeshes->item(i)->text() != m_glWidget->subMeshName(i)) { changed = true; break; }
    }
    if (!changed) return;
    QSignalBlocker blocker(m_subMeshes);
    m_subMeshes->clear();
    for (int i = 0; i < count; ++i) {
        auto* item = new QListWidgetItem(m_glWidget->subMeshName(i), m_subMeshes);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(m_glWidget->subMeshVisible(i) ? Qt::Checked : Qt::Unchecked);
    }
    m_subMeshCount = count;
    m_subMeshes->setFixedHeight(std::min(180, count * 25 + (count > 0 ? 2 : 0)));
    const int selected = m_glWidget->selectedSubMesh();
    if (selected >= 0 && selected < count)
        m_subMeshes->setCurrentRow(selected);
    }

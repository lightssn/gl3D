#include "debugwindow.h"
#include "glwidget.h"
#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

class FrameGraph : public QWidget {
public:
    explicit FrameGraph(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(110); }
    void setSamples(const QVector<float>& v) { m_values = v; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), QColor("#161a22"));
        p.setPen(QColor("#394253")); p.drawLine(0, height() - (int)(16.67f / 50.0f * height()), width(), height() - (int)(16.67f / 50.0f * height()));
        p.setPen(QColor("#63d6a0")); if (m_values.size() < 2) return;
        QPolygonF poly; for (int i = 0; i < m_values.size(); ++i) poly << QPointF((float)i / (m_values.size() - 1) * width(), height() - std::min(m_values[i], 50.0f) / 50.0f * height());
        p.drawPolyline(poly);
    }
private: QVector<float> m_values;
};

static QString bytesText(qint64 n) { return n < 1024 ? QString("%1 B").arg(n) : n < 1048576 ? QString("%1 KiB").arg(n / 1024.0, 0, 'f', 2) : QString("%1 MiB").arg(n / 1048576.0, 0, 'f', 2); }

DebugWindow::DebugWindow(GLWidget* glWidget, QWidget* parent) : QDialog(parent), m_glWidget(glWidget) {
    setWindowTitle("OpenGL 调试器"); resize(650, 560); auto* root = new QVBoxLayout(this);
    auto* box = new QGroupBox("渲染选项", this); auto* layout = new QGridLayout(box);
    auto* msaa = new QCheckBox("抗锯齿 (MSAA)", box); auto* mipmaps = new QCheckBox("纹理 Mipmap", box); auto* grid = new QCheckBox("网格", box); auto* gizmo = new QCheckBox("操作器", box); auto* selection = new QCheckBox("选中描边", box);
    msaa->setChecked(true); mipmaps->setChecked(false); grid->setChecked(true); gizmo->setChecked(true); selection->setChecked(true);
    layout->addWidget(msaa, 0, 0); layout->addWidget(mipmaps, 0, 1); layout->addWidget(grid, 0, 2); layout->addWidget(gizmo, 0, 3); layout->addWidget(selection, 0, 4); root->addWidget(box);
    connect(msaa, &QCheckBox::toggled, glWidget, &GLWidget::setAntialiasing); connect(mipmaps, &QCheckBox::toggled, glWidget, &GLWidget::setMipmaps); connect(grid, &QCheckBox::toggled, glWidget, &GLWidget::setShowGrid); connect(gizmo, &QCheckBox::toggled, glWidget, &GLWidget::setShowGizmo); connect(selection, &QCheckBox::toggled, glWidget, &GLWidget::setShowSelection);
    m_cameraLabel = new QLabel(this); m_timingLabel = new QLabel(this); root->addWidget(m_cameraLabel); root->addWidget(m_timingLabel); m_graph = new FrameGraph(this); root->addWidget(m_graph);
    m_resources = new QTableWidget(0, 3, this); m_resources->setHorizontalHeaderLabels({"对象", "数量", "显存占用"}); m_resources->horizontalHeader()->setStretchLastSection(true); m_resources->setEditTriggers(QAbstractItemView::NoEditTriggers); root->addWidget(m_resources, 1);
    m_timer = new QTimer(this); connect(m_timer, &QTimer::timeout, this, &DebugWindow::refresh); m_timer->start(250); refresh();
}

void DebugWindow::refresh() {
    const DebugSnapshot s = m_glWidget->debugSnapshot();
    m_cameraLabel->setText(QString("相机：%1  FOV：%2°  距离：%3\n眼睛：( %4, %5, %6 )  目标：( %7, %8, %9 )").arg(s.ortho ? "正交" : "透视").arg(s.fov, 0, 'f', 1).arg(s.distance, 0, 'f', 3).arg(s.eye.x(), 0, 'f', 2).arg(s.eye.y(), 0, 'f', 2).arg(s.eye.z(), 0, 'f', 2).arg(s.target.x(), 0, 'f', 2).arg(s.target.y(), 0, 'f', 2).arg(s.target.z(), 0, 'f', 2));
    m_timingLabel->setText(QString("FPS：%1    平均帧：%2 ms    最慢帧：%3 ms（曲线，基准 16.67 ms）").arg(s.fps).arg(s.averageFrameMs, 0, 'f', 2).arg(s.lowFrameMs, 0, 'f', 2)); m_graph->setSamples(s.frameTimesMs);
    m_resources->setRowCount(s.resources.size()); for (int i = 0; i < s.resources.size(); ++i) { const auto& r = s.resources[i]; m_resources->setItem(i, 0, new QTableWidgetItem(r.type)); m_resources->setItem(i, 1, new QTableWidgetItem(QString::number(r.count))); m_resources->setItem(i, 2, new QTableWidgetItem(r.queryable ? bytesText(r.bytes) : "GL 不暴露对象元数据大小")); }
}

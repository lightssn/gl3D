#pragma once
#include "debugstats.h"
#include <QWidget>
class QLabel; class QTableWidget; class FrameGraph; class GLWidget; class QTimer; class QCheckBox;
class DebugWindow : public QWidget {
    Q_OBJECT
public:
    explicit DebugWindow(GLWidget* glWidget, QWidget* parent = nullptr);
    void syncWireframe(bool enabled);
    void setNightMode(bool night);
signals:
    void wireframeChanged(bool enabled);
private:
    void refresh();
    GLWidget* m_glWidget; QLabel* m_driverLabel; QLabel* m_cameraLabel; QLabel* m_timingLabel;
    QTableWidget* m_resources; FrameGraph* m_graph; QTimer* m_timer;
    QCheckBox* m_wireframe;
};

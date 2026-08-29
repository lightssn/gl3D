#pragma once
#include "debugstats.h"
#include <QWidget>
class QLabel; class QTableWidget; class FrameGraph; class GLWidget; class QTimer; class QListWidget; class QShowEvent; class QDoubleSpinBox;
class DebugWindow : public QWidget {
    Q_OBJECT
public:
    explicit DebugWindow(GLWidget* glWidget, QWidget* parent = nullptr);
    void setNightMode(bool night);
protected:
    void showEvent(QShowEvent* event) override;
private:
    void refresh();
    void refreshSubMeshes();
    GLWidget* m_glWidget; QLabel* m_driverLabel; QLabel* m_cameraLabel; QLabel* m_timingLabel;
    QTableWidget* m_resources; FrameGraph* m_graph; QTimer* m_timer;
    QListWidget* m_subMeshes;
    QDoubleSpinBox* m_outlineWidth;
    int m_subMeshCount = -1;
};

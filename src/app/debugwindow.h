#pragma once
#include "debugstats.h"
#include <QDialog>
class QCheckBox; class QLabel; class QTableWidget; class FrameGraph; class GLWidget; class QTimer;
class DebugWindow : public QDialog {
    Q_OBJECT
public:
    explicit DebugWindow(GLWidget* glWidget, QWidget* parent = nullptr);
private:
    void refresh();
    GLWidget* m_glWidget; QLabel* m_cameraLabel; QLabel* m_timingLabel;
    QTableWidget* m_resources; FrameGraph* m_graph; QTimer* m_timer;
};

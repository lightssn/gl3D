#pragma once
#include "debugstats.h"
#include <QWidget>
class QLabel; class QTableWidget; class FrameGraph; class RenderView; class QTimer; class QListWidget; class QShowEvent; class QDoubleSpinBox; class QCheckBox; class QComboBox; class QSpinBox;
class DebugWindow : public QWidget {
    Q_OBJECT
public:
    explicit DebugWindow(RenderView* view, QWidget* parent = nullptr);
    void setRenderView(RenderView* view);
    void setNightMode(bool night);
protected:
    void showEvent(QShowEvent* event) override;
private:
    void refresh();
    void refreshSubMeshes();
    RenderView* m_renderView; QLabel* m_driverLabel; QLabel* m_cameraLabel; QLabel* m_timingLabel;
    QTableWidget* m_resources; FrameGraph* m_graph; QTimer* m_timer;
    QListWidget* m_subMeshes;
    QDoubleSpinBox* m_outlineWidth;
    QCheckBox *m_msaa, *m_mipmap, *m_depth, *m_culling, *m_pbr, *m_normalMap, *m_fixedFps;
    QComboBox* m_debugView;
    QSpinBox* m_fpsSpin;
    int m_subMeshCount = -1;
};

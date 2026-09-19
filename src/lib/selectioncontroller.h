#pragma once

#include <QVector3D>
#include <vector>

// Backend-neutral selection state shared by the view and future renderers.
class SelectionController {
public:
    int index() const { return m_index; }
    int& indexRef() { return m_index; }
    std::vector<QVector3D>& centers() { return m_centers; }
    const std::vector<QVector3D>& centers() const { return m_centers; }

private:
    int m_index = -1;
    std::vector<QVector3D> m_centers;
};

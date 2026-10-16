#pragma once
#include "gl3d_export.h"
#include <QMatrix4x4>
#include <QQuaternion>
#include <QVector3D>
#include <map>
#include <vector>

struct GL3D_EXPORT EditTransform {
    QVector3D position;
    QQuaternion rotation;
    QVector3D scale{1,1,1};
    QMatrix4x4 matrix(const QVector3D& center) const;
};

// Shared editor state; window-specific ray/projection input stays in each view.
class GL3D_EXPORT EditController {
public:
    int mode = 0;
    enum class ChangePolicy { ComponentTolerance, VectorTolerance };
    EditTransform& current() { return state(m_selected); }
    const EditTransform& current() const { return state(m_selected); }
    std::vector<EditTransform>& states() { return m_states; }
    void reset();
    void clearHistory();
    void selectState(int index);
    EditTransform& state(int index);
    const EditTransform& state(int index) const;
    void resizeObjects(size_t count);
    void setOwner(int index, int owner);
    void beginDrag(const EditTransform& state) { m_dragStart=state; }
    const EditTransform& dragStart() const { return m_dragStart; }
    bool endDrag(int object,const EditTransform& state,ChangePolicy policy) { return commit(object,m_dragStart,state,policy); }
    bool commit(int object,const EditTransform& before,const EditTransform& after,ChangePolicy policy);
    bool undo(int object,EditTransform& state);
    bool redo(int object,EditTransform& state);
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    EditTransform tileState(int owner) const;
    void setTileState(int owner,const EditTransform& state) { m_tiles[owner]=state; }
    static QVector3D axis(int index);
    static QVector3D gizmoAxis(int index,bool local,const QQuaternion& rotation);
    static float angleDelta(float angle,float previous);
    static void translate(EditTransform& state,const QVector3D& axis,float distance);
    static QQuaternion rotate(const QQuaternion& rotation,const QVector3D& axis,float degrees);
    static float rayScale(float initial,float parameter,float anchor);
    static float screenScale(float current,float along);
private:
    struct Command { int object; EditTransform before,after; };
    static bool changed(const EditTransform& before,const EditTransform& after,ChangePolicy policy);
    int m_selected = -1;
    EditTransform m_empty;
    std::vector<int> m_owners;
    EditTransform m_dragStart;
    std::vector<EditTransform> m_states;
    std::map<int,EditTransform> m_tiles;
    std::vector<Command> m_undo,m_redo;
};

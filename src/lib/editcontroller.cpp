#include "editcontroller.h"
#include <algorithm>
#include <cmath>

QMatrix4x4 EditTransform::matrix(const QVector3D& center) const {
    QMatrix4x4 result;
    result.translate(center+position); result.rotate(rotation); result.scale(scale); result.translate(-center);
    return result;
}
void EditController::clearHistory() { m_undo.clear(); m_redo.clear(); }
void EditController::selectState(int index) {
    m_selected=index>=0 && index<int(m_states.size())?index:-1;
    m_empty={};
    clearHistory();
}
void EditController::resizeObjects(size_t count) {
    m_states.resize(count);
    m_owners.resize(count,-1);
}
void EditController::setOwner(int index,int owner) {
    if(index>=0 && index<int(m_owners.size())) m_owners[size_t(index)]=owner;
}
EditTransform& EditController::state(int index) {
    if(index<0 || index>=int(m_states.size())) return m_empty;
    if(index<int(m_owners.size()) && m_owners[size_t(index)]>=0) return m_tiles[m_owners[size_t(index)]];
    return m_states[size_t(index)];
}
const EditTransform& EditController::state(int index) const {
    if(index<0 || index>=int(m_states.size())) return m_empty;
    if(index<int(m_owners.size()) && m_owners[size_t(index)]>=0) {
        const auto found=m_tiles.find(m_owners[size_t(index)]);
        return found==m_tiles.end()?m_empty:found->second;
    }
    return m_states[size_t(index)];
}
void EditController::reset() {
    m_selected=-1; m_empty={}; m_dragStart={}; m_states.clear(); m_owners.clear(); m_tiles.clear(); clearHistory();
}
bool EditController::changed(const EditTransform& a,const EditTransform& b,ChangePolicy policy) {
    if((a.position-b.position).length()>1e-5f) return true;
    // Preserve both existing backends' no-op thresholds, including quaternion signs.
    if(policy==ChangePolicy::VectorTolerance)
        return (a.scale-b.scale).length()>1e-5f || a.rotation!=b.rotation;
    if(std::fabs(a.scale.x()-b.scale.x())>1e-5f || std::fabs(a.scale.y()-b.scale.y())>1e-5f || std::fabs(a.scale.z()-b.scale.z())>1e-5f) return true;
    const float difference=std::fabs(a.rotation.x()-b.rotation.x())+std::fabs(a.rotation.y()-b.rotation.y())+
        std::fabs(a.rotation.z()-b.rotation.z())+std::fabs(a.rotation.scalar()-b.rotation.scalar());
    return !(difference<1e-5f);
}
bool EditController::commit(int object,const EditTransform& before,const EditTransform& after,ChangePolicy policy) {
    if(!changed(before,after,policy)) return false;
    if(!m_undo.empty() && m_undo.back().object!=object) clearHistory();
    m_undo.push_back({object,before,after}); m_redo.clear(); return true;
}
bool EditController::undo(int object,EditTransform& state) {
    if(m_undo.empty() || m_undo.back().object!=object) return false;
    auto command=m_undo.back(); m_undo.pop_back(); command.after=state; state=command.before; m_redo.push_back(command); return true;
}
bool EditController::redo(int object,EditTransform& state) {
    if(m_redo.empty() || m_redo.back().object!=object) return false;
    auto command=m_redo.back(); m_redo.pop_back(); command.before=state; state=command.after; m_undo.push_back(command); return true;
}
EditTransform EditController::tileState(int owner) const {
    const auto found=m_tiles.find(owner); return found==m_tiles.end()?EditTransform():found->second;
}
QVector3D EditController::axis(int index) { return index==1?QVector3D(1,0,0):index==2?QVector3D(0,1,0):QVector3D(0,0,1); }
QVector3D EditController::gizmoAxis(int index,bool local,const QQuaternion& rotation) {
    const auto direction=axis(index); return local?rotation.rotatedVector(direction):direction;
}
float EditController::angleDelta(float angle,float previous) {
    float difference=angle-previous;
    if(difference>3.14159265f) difference-=6.2831853f;
    if(difference<-3.14159265f) difference+=6.2831853f;
    return difference;
}
void EditController::translate(EditTransform& state,const QVector3D& direction,float distance) { state.position+=direction*distance; }
QQuaternion EditController::rotate(const QQuaternion& rotation,const QVector3D& direction,float degrees) {
    return QQuaternion::fromAxisAndAngle(direction,degrees)*rotation;
}
float EditController::rayScale(float initial,float parameter,float anchor) {
    return initial*std::clamp(std::fabs(parameter)/std::fabs(anchor),0.05f,20.0f);
}
float EditController::screenScale(float current,float along) { return std::clamp(current*(1.0f+along/120.0f),0.02f,100.0f); }

#pragma once
#include "gl3d_export.h"
#include <QtGlobal>
struct ProcessMemory {
    qint64 workingSet=0, privateWorkingSet=0, privateCommit=0;
    bool valid=false, privateWorkingSetValid=false;
};
GL3D_EXPORT ProcessMemory processMemory();

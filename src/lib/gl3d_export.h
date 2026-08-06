#pragma once

// 动态库导入导出宏 编译库时定义GL3D_CORE_BUILD
#ifdef _WIN32
    #ifdef GL3D_CORE_BUILD
        #define GL3D_EXPORT __declspec(dllexport)
    #else
        #define GL3D_EXPORT __declspec(dllimport)
    #endif
#else
    #define GL3D_EXPORT
#endif

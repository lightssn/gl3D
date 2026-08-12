#pragma once
#include "gl3d_export.h"
#include <QOpenGLFunctions_4_5_Core>

//单例封装QOpenGLFunctions_4_5_Core 统一管理GL函数指针生命周期
//Qt最高仅到4_5 而OpenGL4.6未新增API入口点 故4_5_Core覆盖4.6全部函数
//堆分配+显式销毁 避免静态析构阶段context已销毁仍访问函数表
//用法 GLWidget::initializeGL内init 任意处instance调用 析构前destroy
class GL3D_EXPORT GLFunctions : public QOpenGLFunctions_4_5_Core {
    GLFunctions() = default;
    ~GLFunctions() override = default;
    inline static GLFunctions* s_instance = nullptr;

public:
    static bool init() {
        if (!s_instance) {
            s_instance = new GLFunctions();
            return s_instance->initializeOpenGLFunctions();
        }
        return true;
    }

    static GLFunctions& instance() { return *s_instance; }

    //须在context销毁前调用 由GLWidget析构负责
    static void destroy() {
        delete s_instance;
        s_instance = nullptr;
    }
};

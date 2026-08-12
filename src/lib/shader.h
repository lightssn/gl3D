#pragma once
#include "gl3d_export.h"

//着色器程序封装 编译链接uniform设置 
class GL3D_EXPORT Shader {
    unsigned int compileStage(unsigned int type, const char* source);
    void cleanup();
    unsigned int m_program = 0;
public:
    Shader() = default;
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;//禁止拷贝

    //编译链接 失败打印info log
    bool loadFromSource(const char* vertSrc, const char* fragSrc);

    void bind() const;
    void unbind() const;

    void setMat4(const char* name, const float* mat) const;
    void setVec3(const char* name, float x, float y, float z) const;
    void setVec4(const char* name, float x, float y, float z, float w) const;
    void setFloat(const char* name, float val) const;
    void setInt(const char* name, int val) const;

    bool isValid() const { return m_program != 0; }
};

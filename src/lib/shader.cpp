#include "shader.h"
#include "glcontext.h"
#include <cstdio>

Shader::~Shader() { cleanup(); }

void Shader::cleanup() {
    if (m_program) {
        GLFunctions::instance().glDeleteProgram(m_program);
        m_program = 0;
    }
}

unsigned int Shader::compileStage(unsigned int type, const char* source) {
    auto& gl = GLFunctions::instance();
    GLuint sh = gl.glCreateShader(type);
    gl.glShaderSource(sh, 1, &source, nullptr);
    gl.glCompileShader(sh);
    GLint ok = 0;
    gl.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        printf("[Shader] %s编译失败: %s\n", type == GL_VERTEX_SHADER ? "顶点" : "片段", log);
        gl.glDeleteShader(sh);
        return 0;
    }
    return sh;
}

bool Shader::loadFromSource(const char* vertSrc, const char* fragSrc) {
    cleanup();
    auto& gl = GLFunctions::instance();
    GLuint vert = compileStage(GL_VERTEX_SHADER, vertSrc);
    GLuint frag = compileStage(GL_FRAGMENT_SHADER, fragSrc);
    if (!vert || !frag) {
        if (vert) gl.glDeleteShader(vert);
        if (frag) gl.glDeleteShader(frag);
        return false;
    }

    m_program = gl.glCreateProgram();
    gl.glAttachShader(m_program, vert);
    gl.glAttachShader(m_program, frag);
    gl.glLinkProgram(m_program);
    gl.glDeleteShader(vert); //链接后着色器对象即可释放
    gl.glDeleteShader(frag);

    GLint ok = 0;
    gl.glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
        printf("[Shader] 链接失败: %s\n", log);
        cleanup();
        return false;
    }
    return true;
}

void Shader::bind() const { GLFunctions::instance().glUseProgram(m_program); }
void Shader::unbind() const { GLFunctions::instance().glUseProgram(0); }

void Shader::setMat4(const char* name, const float* mat) const {
    auto& gl = GLFunctions::instance();
    gl.glUniformMatrix4fv(gl.glGetUniformLocation(m_program, name), 1, GL_FALSE, mat);
}
void Shader::setVec3(const char* name, float x, float y, float z) const {
    auto& gl = GLFunctions::instance();
    gl.glUniform3f(gl.glGetUniformLocation(m_program, name), x, y, z);
}
void Shader::setVec4(const char* name, float x, float y, float z, float w) const {
    auto& gl = GLFunctions::instance();
    gl.glUniform4f(gl.glGetUniformLocation(m_program, name), x, y, z, w);
}
void Shader::setFloat(const char* name, float val) const {
    auto& gl = GLFunctions::instance();
    gl.glUniform1f(gl.glGetUniformLocation(m_program, name), val);
}
void Shader::setInt(const char* name, int val) const {
    auto& gl = GLFunctions::instance();
    gl.glUniform1i(gl.glGetUniformLocation(m_program, name), val);
}

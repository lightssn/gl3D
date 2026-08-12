#include "glwidget.h"
#include "glcontext.h"
#include "shader.h"
#include "meshrenderer.h"
#include "meshloader.h"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QVector4D>
#include <algorithm>
#include <cmath>
#include <cstdio>

static const float kPi = 3.14159265f;

//顶点着色器
static const char* VERT_SRC = R"(
#version 460 core
layout(location = 0) in vec3 aPos; //位置
layout(location = 1) in vec3 aNormal; //模型空间法线
layout(location = 2) in vec2 aUV; //纹理坐标
//输入和glVertexAttribPointer一致

uniform mat4 uModel; //模型矩阵T·R·S
uniform mat4 uView; //视图矩阵
uniform mat4 uProj; //投影矩阵
uniform float uOutline; //描边量，0为不描边

out vec3 vNormal; //观察空间法线
out vec2 vUV; //透传uv

void main() {
    vec4 wp = uModel * vec4(aPos, 1.0); //模型顶点→世界顶点
    vec3 wn = mat3(uModel) * aNormal; //世界法线，mat3去缩放
    if (uOutline > 0.0) wp.xyz += wn * uOutline; //顶点沿法线外推，生成选中外轮廓
    gl_Position = uProj * uView * wp; //glsl规范不写out
    vNormal = mat3(transpose(inverse(uView))) * wn; //观察法线=视图矩阵inverse求逆去缩放+transpose转置确保垂直+mat3去平移*世界法线
    vUV = aUV; //光栅化插值
}
)";

//片段着色器
static const char* FRAG_SRC = R"(
#version 460 core
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTexture; //纹理，glGenTextures / glBindTexture / glTexImage2D创建
uniform int uUseTexture; //纹理开关
uniform vec3 uDiffuse; //纯色底色
uniform int uFlatColor; //纯色开关
uniform vec4 uFlatColorValue; //含alpha纯色值

void main() {
    //选中红边/粉色蒙版，跳过光照
    if (uFlatColor == 1) { FragColor = uFlatColorValue; return; }

    vec3 base = uUseTexture == 1 ? texture(uTexture, vUV).rgb : uDiffuse; //底色=纹理rgb或纯色
    vec3 n = normalize(vNormal); //插值后法线≠1 重归一化
    vec3 light = vec3(0.0, 0.0, 1.0); //头灯，视线z
    float diff = abs(dot(n, light)) * 0.75; //漫反射，dot(n, light)为法线和光线夹角余弦值，-1背黑~0侧中~1正亮，abs兼容单面片
    vec3 h = normalize(light + vec3(0.0, 0.0, 1.0)); //半程向量
    float spec = pow(max(abs(dot(n, h)), 0.0), 32.0) * 0.25; //高光锐度32，压制高光0.25
    vec3 color = base * (0.30 + diff) + vec3(spec); //环境光0.3
    FragColor = vec4(color, 1.0);
}
)";

//线段-顶点着色器
static const char* OVERLAY_VERT = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
uniform int uUseVertexColor; //顶点色/统一色，当前只传顶点色
uniform vec3 uColor;
out vec3 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = uUseVertexColor == 1 ? aColor : uColor;
}
)";

//线段-片段着色器
static const char* OVERLAY_FRAG = R"(
#version 460 core
in vec3 vColor;
out vec4 FragColor;
void main() { FragColor = vec4(vColor, 1.0); }
)";

GLWidget::GLWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setFocusPolicy(Qt::StrongFocus); //接收键盘
    m_fpsTimer.start();
    }

GLWidget::~GLWidget() {
    //context仍有效时释放GL资源 再销毁函数指针单例
    makeCurrent();
    m_renderer.reset();
    m_shader.reset();
    m_overlayShader.reset();
    releaseOverlay(m_gridMesh);
    releaseOverlay(m_axesMesh);
    releaseOverlay(m_gizmoMesh);
    auto& gl = GLFunctions::instance();
    if (m_pickFbo) gl.glDeleteFramebuffers(1, &m_pickFbo);
    if (m_pickColor) gl.glDeleteTextures(1, &m_pickColor);
    if (m_pickDepth) gl.glDeleteRenderbuffers(1, &m_pickDepth);
    doneCurrent();
    GLFunctions::destroy();
    }

void GLWidget::initializeGL() {
    GLFunctions::init();
    auto& gl = GLFunctions::instance();
    gl.glEnable(GL_DEPTH_TEST);
    gl.glDisable(GL_CULL_FACE); //叶片等薄片需双面可见
    gl.glEnable(GL_STENCIL_TEST); //选中描边依赖stencil
    gl.glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], 1.0f);
    m_dpr = devicePixelRatioF();
    printf("[GLWidget] %s\n", (const char*)gl.glGetString(GL_RENDERER));

    m_shader = std::make_unique<Shader>();
    m_shader->loadFromSource(VERT_SRC, FRAG_SRC);
    m_overlayShader = std::make_unique<Shader>();
    m_overlayShader->loadFromSource(OVERLAY_VERT, OVERLAY_FRAG);
    m_renderer = std::make_unique<MeshRenderer>();

    buildAxesMesh();
    createPickTarget(std::max((int)(width() * m_dpr), 1), std::max((int)(height() * m_dpr), 1));

    //构造函数期间收到过加载请求 此时补传显存
    if (!m_mesh.subMeshes.empty()) {
        m_renderer->upload(m_mesh);
        refreshOverlays();
        }
    }

void GLWidget::resizeGL(int w, int h) {
    auto& gl = GLFunctions::instance();
    gl.glViewport(0, 0, w, h);
    m_fbW = w;
    m_fbH = h;
    createPickTarget(w, h);
    }

void GLWidget::paintGL() {
    auto& gl = GLFunctions::instance();
    gl.glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], 1.0f);
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    //FPS统计 每秒发一次
    ++m_frameCount;
    if (m_fpsTimer.elapsed() >= 1000) {
        emit fpsUpdated(m_frameCount);
        m_frameCount = 0;
        m_fpsTimer.restart();
        }

    if (!m_shader || !m_shader->isValid() || !hasModel()) return;

    QMatrix4x4 view = m_camera.viewMatrix();
    float aspect = height() > 0 ? (float)width() / height() : 1.0f;
    QMatrix4x4 proj = m_camera.projMatrix(aspect);

    drawGrid(proj, view); //背景网格 先画被模型遮挡

    //主模型 Blinn-Phong头灯
    m_shader->bind();
    m_shader->setMat4("uView", view.constData());
    m_shader->setMat4("uProj", proj.constData());
    m_shader->setMat4("uModel", m_modelMatrix.constData());
    m_shader->setFloat("uOutline", 0.0f);
    m_shader->setInt("uFlatColor", 0);
    m_shader->setVec4("uFlatColorValue", 1, 1, 1, 1);
    m_renderer->render(*m_shader);

    if (m_selected) drawSelection(proj, view); //红边+粉色蒙版
    m_shader->unbind();

    //操作器与坐标轴HUD 顶层显示 关深度测试
    gl.glDisable(GL_DEPTH_TEST);
    if (m_transformMode != None) drawGizmo(proj, view);
    drawAxesHud(view);
    gl.glEnable(GL_DEPTH_TEST);
    }

//XY平面(z=0)网格 平行X与平行Y两组线 坐标轴加亮
void GLWidget::rebuildGrid() {
    if (!m_overlayShader || !m_overlayShader->isValid()) return;
    float radius = m_mesh.radius();
    if (radius <= 0.0f) return;
    std::vector<OverlayVertex> verts;
    //网格色随主题: 深底用亮灰 浅底用深灰
    float lum = (m_clearColor[0] + m_clearColor[1] + m_clearColor[2]) / 3.0f;
    QVector3D base = lum > 0.5f ? QVector3D(0.45f, 0.45f, 0.5f) : QVector3D(0.32f, 0.32f, 0.38f);
    QVector3D strong = base * 1.7f;
    float ext = radius * 1.5f;
    float eps = -ext * 0.001f; //网格整体后移 避免与z=0面片深度打架
    const int lines = 8; //单方向线数 疏密适中
    float step = 2.0f * ext / lines;
    for (int i = 0; i <= lines; ++i) {
        float p = -ext + i * step;
        pushLine(verts, QVector3D(-ext, p, eps), QVector3D(ext, p, eps), base);
        pushLine(verts, QVector3D(p, -ext, eps), QVector3D(p, ext, eps), base);
        }
    pushLine(verts, QVector3D(-ext, 0, eps), QVector3D(ext, 0, eps), strong);
    pushLine(verts, QVector3D(0, -ext, eps), QVector3D(0, ext, eps), strong);
    uploadOverlay(m_gridMesh, verts);
    }

//左下角XYZ轴HUD 单位长度箭头 渲染时仅取视角旋转 固定在左下角
void GLWidget::buildAxesMesh() {
    if (!m_overlayShader || !m_overlayShader->isValid()) return;
    std::vector<OverlayVertex> verts;
    pushAxisArrow(verts, QVector3D(1, 0, 0), 1.0f, QVector3D(0.95f, 0.25f, 0.25f));
    pushAxisArrow(verts, QVector3D(0, 1, 0), 1.0f, QVector3D(0.25f, 0.95f, 0.25f));
    pushAxisArrow(verts, QVector3D(0, 0, 1), 1.0f, QVector3D(0.3f, 0.5f, 1.0f));
    uploadOverlay(m_axesMesh, verts);
    }

//模型中央操作器 平移=箭头 旋转=圆环 缩放=箭头+方框手柄
void GLWidget::rebuildGizmo() {
    if (!m_overlayShader || !m_overlayShader->isValid()) return;
    std::vector<OverlayVertex> verts;
    float s = m_mesh.radius() * 1.2f;
    QVector3D red(0.95f, 0.25f, 0.25f), green(0.25f, 0.95f, 0.25f), blue(0.3f, 0.5f, 1.0f);
    QVector3D x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
    if (m_transformMode == Translate) {
        pushAxisArrow(verts, x, s, red);
        pushAxisArrow(verts, y, s, green);
        pushAxisArrow(verts, z, s, blue);
        }
    else if (m_transformMode == Rotate) {
        pushCircle(verts, QVector3D(), x, s, red); //绕X旋转 圆在YZ平面
        pushCircle(verts, QVector3D(), y, s, green);
        pushCircle(verts, QVector3D(), z, s, blue);
        }
    else if (m_transformMode == Scale) {
        //缩放箭头随模型旋转 对齐模型本地坐标轴
        QVector3D rx = m_transformRot.rotatedVector(x);
        QVector3D ry = m_transformRot.rotatedVector(y);
        QVector3D rz = m_transformRot.rotatedVector(z);
        pushAxisArrow(verts, rx, s, red);
        pushAxisArrow(verts, ry, s, green);
        pushAxisArrow(verts, rz, s, blue);
        pushWireCube(verts, rx * s, s * 0.18f, red); //轴端方框 区别平移
        pushWireCube(verts, ry * s, s * 0.18f, green);
        pushWireCube(verts, rz * s, s * 0.18f, blue);
        }
    uploadOverlay(m_gizmoMesh, verts);
    }

void GLWidget::refreshOverlays() {
    rebuildGrid();
    if (m_transformMode != None) rebuildGizmo();
    }

//网格/坐标轴/操作器几何统一上传 交错pos3+color3
void GLWidget::uploadOverlay(OverlayMesh& mesh, const std::vector<OverlayVertex>& verts) {
    releaseOverlay(mesh);
    if (verts.empty()) return;
    auto& gl = GLFunctions::instance();
    gl.glGenVertexArrays(1, &mesh.vao);
    gl.glGenBuffers(1, &mesh.vbo);
    gl.glBindVertexArray(mesh.vao);
    gl.glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(OverlayVertex)),
                    verts.data(), GL_STATIC_DRAW);
    gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), (void*)0);
    gl.glEnableVertexAttribArray(0);
    gl.glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex),
                             (void*)(3 * sizeof(float)));
    gl.glEnableVertexAttribArray(1);
    gl.glBindVertexArray(0);
    mesh.count = (int)verts.size();
    }

void GLWidget::releaseOverlay(OverlayMesh& mesh) {
    if (!mesh.vao) return;
    auto& gl = GLFunctions::instance();
    gl.glDeleteVertexArrays(1, &mesh.vao);
    gl.glDeleteBuffers(1, &mesh.vbo);
    mesh.vao = mesh.vbo = 0;
    mesh.count = 0;
    }

void GLWidget::drawOverlay(const OverlayMesh& mesh, const QMatrix4x4& mvp,
                           bool useVertexColor, const QVector3D& color) {
    if (!mesh.count || !m_overlayShader || !m_overlayShader->isValid()) return;
    auto& gl = GLFunctions::instance();
    m_overlayShader->bind();
    m_overlayShader->setMat4("uMVP", mvp.constData());
    m_overlayShader->setInt("uUseVertexColor", useVertexColor ? 1 : 0);
    m_overlayShader->setVec3("uColor", color.x(), color.y(), color.z());
    gl.glBindVertexArray(mesh.vao);
    gl.glDrawArrays(GL_LINES, 0, mesh.count);
    gl.glBindVertexArray(0);
    m_overlayShader->unbind();
    }

void GLWidget::drawGrid(const QMatrix4x4& proj, const QMatrix4x4& view) {
    if (!m_gridMesh.count) return;
    drawOverlay(m_gridMesh, proj * view, true, QVector3D());
    }

//选中模型: stencil描红边 + 浅粉半透明蒙版
void GLWidget::drawSelection(const QMatrix4x4& proj, const QMatrix4x4& view) {
    auto& gl = GLFunctions::instance();
    m_shader->setMat4("uView", view.constData());
    m_shader->setMat4("uProj", proj.constData());
    m_shader->setMat4("uModel", m_modelMatrix.constData());
    m_shader->setInt("uFlatColor", 1);

    //1 选中模型写入stencil 颜色忽略 深度用LEQUAL同深度通过
    gl.glEnable(GL_STENCIL_TEST);
    gl.glStencilFunc(GL_ALWAYS, 1, 0xFF);
    gl.glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    gl.glStencilMask(0xFF);
    gl.glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    gl.glDepthFunc(GL_LEQUAL);
    m_shader->setFloat("uOutline", 0.0f);
    m_renderer->render(*m_shader);
    gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    //2 沿法线膨胀画红边 只画stencil未覆盖的外缘
    m_shader->setFloat("uOutline", m_outlineWidth);
    m_shader->setVec4("uFlatColorValue", 0.9f, 0.1f, 0.1f, 1.0f);
    gl.glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    gl.glStencilMask(0x00);
    gl.glDisable(GL_DEPTH_TEST);
    m_renderer->render(*m_shader);
    gl.glEnable(GL_DEPTH_TEST);
    gl.glStencilMask(0xFF);
    gl.glDisable(GL_STENCIL_TEST);

    //3 浅粉色半透明蒙版 LEQUAL避免同深度被剔除
    gl.glEnable(GL_BLEND);
    gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_shader->setFloat("uOutline", 0.0f);
    m_shader->setVec4("uFlatColorValue", 1.0f, 0.76f, 0.82f, 0.35f);
    gl.glDepthMask(GL_FALSE);
    gl.glDepthFunc(GL_LEQUAL);
    m_renderer->render(*m_shader);
    gl.glDepthFunc(GL_LESS);
    gl.glDepthMask(GL_TRUE);
    gl.glDisable(GL_BLEND);
    }

void GLWidget::drawGizmo(const QMatrix4x4& proj, const QMatrix4x4& view) {
    QMatrix4x4 mvp = proj * view;
    mvp.translate(m_mesh.center() + m_transformPos); //操作器定位在变换后的模型中央
    drawOverlay(m_gizmoMesh, mvp, true, QVector3D());
    }

//坐标轴HUD 取视图旋转(去掉平移) 经缩放位移固定到左下角
void GLWidget::drawAxesHud(const QMatrix4x4& view) {
    QMatrix4x4 viewRot = view;
    viewRot.setColumn(3, QVector4D(0, 0, 0, 1)); //只留旋转 轴随视角转动
    QMatrix4x4 mvp;
    mvp.ortho(-1, 1, -1, 1, -100, 100); //单位盒子映射全屏
    mvp = mvp * viewRot;
    QMatrix4x4 corner; //缩放到左下角 中心约NDC(-0.8,-0.8)
    corner.translate(-0.8f, -0.8f, 0.0f);
    corner.scale(0.2f);
    mvp = corner * mvp;
    drawOverlay(m_axesMesh, mvp, true, QVector3D());
    }

//离屏拾取目标 与视口同尺寸 颜色+深度附件
void GLWidget::createPickTarget(int w, int h) {
    if (w <= 0 || h <= 0) return;
    auto& gl = GLFunctions::instance();
    if (m_pickFbo) gl.glDeleteFramebuffers(1, &m_pickFbo);
    if (m_pickColor) gl.glDeleteTextures(1, &m_pickColor);
    if (m_pickDepth) gl.glDeleteRenderbuffers(1, &m_pickDepth);
    m_pickFbo = m_pickColor = m_pickDepth = 0;
    gl.glGenFramebuffers(1, &m_pickFbo);
    gl.glGenTextures(1, &m_pickColor);
    gl.glBindTexture(GL_TEXTURE_2D, m_pickColor);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.glGenRenderbuffers(1, &m_pickDepth);
    gl.glBindRenderbuffer(GL_RENDERBUFFER, m_pickDepth);
    gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, m_pickFbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_pickColor, 0);
    gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_pickDepth);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    m_pickW = w;
    m_pickH = h;
    }

//左键拾取 模型渲染为纯红 读取点击处像素判断命中
//鼠标事件中GL上下文非当前 须makeCurrent后再操作
void GLWidget::pickAt(const QPoint& pos) {
    if (!hasModel() || !m_pickFbo || !isValid()) {
        update();
        return;
        }
    bool hit = false;
    makeCurrent(); //Qt6返回void 直接使上下文当前
    auto& gl = GLFunctions::instance();
    int x = (int)(pos.x() * m_dpr);
    int y = (int)(pos.y() * m_dpr);
    if (x >= 0 && y >= 0 && x < m_pickW && y < m_pickH) {
        gl.glBindFramebuffer(GL_FRAMEBUFFER, m_pickFbo);
        gl.glViewport(0, 0, m_pickW, m_pickH);
        gl.glClearColor(0, 0, 0, 1);
        gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        m_shader->bind();
        m_shader->setMat4("uView", m_camera.viewMatrix().constData());
        QMatrix4x4 proj = m_camera.projMatrix(m_pickH ? (float)m_pickW / m_pickH : 1.0f);
        m_shader->setMat4("uProj", proj.constData());
        m_shader->setMat4("uModel", m_modelMatrix.constData());
        m_shader->setFloat("uOutline", 0.0f);
        m_shader->setInt("uFlatColor", 1);
        m_shader->setVec4("uFlatColorValue", 1, 0, 0, 1); //命中=红
        m_renderer->render(*m_shader);
        m_shader->unbind();

        unsigned char pixel[4] = { 0, 0, 0, 0 };
        gl.glReadPixels(x, m_pickH - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel); //GL y轴向上
        hit = pixel[0] > 128;
        gl.glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        gl.glViewport(0, 0, m_fbW, m_fbH);
        }
    doneCurrent();
    if (hit != m_selected) { //状态变化才通知 联动工具栏
        m_selected = hit;
        emit selectionChanged(m_selected);
        }
    update();
    }

bool GLWidget::hasModel() const {
    return m_renderer && !m_renderer->empty();
    }

bool GLWidget::loadModel(const QString& path) {
    Mesh mesh;
    QString err;
    if (!MeshLoader::load(path, mesh, &err)) {
        emit loadFailed(err.isEmpty() ? "加载失败" : err);
        return false;
        }

    m_mesh = std::move(mesh);
    if (isValid()) { //context未就绪则留待initializeGL上传
        makeCurrent();
        m_renderer->upload(m_mesh);
        refreshOverlays();
        doneCurrent();
        }
    m_camera.fitToSphere(m_mesh.center(), m_mesh.radius());
    m_outlineWidth = m_mesh.radius() * 0.02f;
    m_transformPos = QVector3D();
    m_transformRot = QQuaternion();
    m_transformScale = QVector3D(1, 1, 1);
    m_dragAxis = 0;
    m_undoStack.clear();
    m_redoStack.clear();
    emit historyChanged(false, false);
    updateModelMatrix();
    m_selected = false;
    emit selectionChanged(false); //新模型默认未选中 操作器按钮隐藏

    QString info = QString("%1  顶点%2  三角%3  子网格%4")
                   .arg(QFileInfo(path).fileName())
                   .arg(m_mesh.vertexCount())
                   .arg(m_mesh.triangleCount())
                   .arg((int)m_mesh.subMeshes.size());
    emit modelLoaded(info);
    update();
    return true;
    }

void GLWidget::resetView() {
    if (!m_mesh.subMeshes.empty())
        m_camera.fitToSphere(m_mesh.center(), m_mesh.radius());
    update();
    }

void GLWidget::setClearColor(float r, float g, float b) {
    //仅记录颜色 initializeGL时生效 context未创建前不可触碰GL
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    if (!isValid()) return;
    makeCurrent();
    auto& gl = GLFunctions::instance();
    gl.glClearColor(r, g, b, 1.0f);
    if (hasModel()) rebuildGrid(); //网格色随主题刷新
    doneCurrent();
    update();
    }

//清除模型 释放GPU资源 隐藏操作器并复位选中状态
void GLWidget::deleteModel() {
    if (!hasModel()) return;
    makeCurrent(); //事件回调中 须使上下文当前才能删GL对象
    m_renderer->clear();
    releaseOverlay(m_gridMesh);
    releaseOverlay(m_gizmoMesh);
    doneCurrent();
    m_mesh = Mesh();
    m_transformPos = QVector3D();
    m_transformRot = QQuaternion();
    m_transformScale = QVector3D(1, 1, 1);
    m_dragAxis = 0;
    m_transformMode = None;
    m_undoStack.clear();
    m_redoStack.clear();
    emit historyChanged(false, false);
    m_selected = false;
    emit selectionChanged(false); //联动隐藏操作器与删除按钮
    emit modelCleared();
    update();
    }

void GLWidget::setOrtho(bool on) {
    m_camera.setOrtho(on); //投影矩阵在paintGL按此标志生成
    update();
    }

void GLWidget::setTransformMode(int mode) {
    m_transformMode = static_cast<TransformMode>(mode);
    m_dragAxis = 0; //切换模式放弃进行中的拖拽
    //工具按钮点击在事件回调中 非绘制期 GL上下文不当前 须makeCurrent再重建手柄
    if (hasModel() && m_overlayShader && m_overlayShader->isValid() && isValid()) {
        makeCurrent();
        rebuildGizmo();
        doneCurrent();
        }
    update();
    }

//组合模型矩阵 T(c+pos)*R*S*T(-c) 绕模型中心缩放旋转 中心跟随位移
void GLWidget::updateModelMatrix() {
    QMatrix4x4 m;
    QVector3D c = m_mesh.center();
    m.translate(c + m_transformPos);
    m.rotate(m_transformRot);
    m.scale(m_transformScale);
    m.translate(-c);
    m_modelMatrix = m;
    }

//由屏幕坐标生成拾取射线 正交时视口平移 透视时从眼位出发
void GLWidget::pickingRay(const QPoint& pos, QVector3D& origin, QVector3D& dir) {
    QMatrix4x4 inv = m_camera.viewMatrix().inverted();
    QVector3D eye = m_camera.eye();
    QVector3D right(inv(0, 0), inv(1, 0), inv(2, 0));
    QVector3D up(inv(0, 1), inv(1, 1), inv(2, 1));
    QVector3D back(inv(0, 2), inv(1, 2), inv(2, 2));
    QVector3D fwd = -back; //相机视线方向
    float w = std::max(width(), 1), h = std::max(height(), 1);
    float sx = 2.0f * pos.x() / w - 1.0f;
    float sy = 1.0f - 2.0f * pos.y() / h;
    float aspect = w / h;
    if (m_camera.ortho()) {
        float halfH = m_camera.distance() * 0.5f;
        float halfW = halfH * aspect;
        origin = eye + right * (sx * halfW) + up * (sy * halfH);
        dir = fwd;
        }
    else {
        float tanHalf = std::tan(45.0f * 0.5f * kPi / 180.0f);
        dir = (right * (sx * aspect * tanHalf) + up * (sy * tanHalf) + fwd).normalized();
        origin = eye;
        }
    }

//世界点→逻辑屏幕坐标 越界也返回 用于操作器像素距离判定
QPointF GLWidget::projectToScreen(const QMatrix4x4& proj, const QMatrix4x4& view, const QVector3D& p) {
    QVector4D clip = proj * view * QVector4D(p, 1.0f);
    if (clip.w() == 0.0f) return QPointF(1e9f, 1e9f);
    float ndcX = clip.x() / clip.w(), ndcY = clip.y() / clip.w();
    float w = std::max(width(), 1), h = std::max(height(), 1);
    return QPointF((ndcX + 1.0f) * 0.5f * w, (1.0f - ndcY) * 0.5f * h);
    }

//射线到过center沿axis直线的最近点 返回沿axis的参数 轴与视线近平行则失败
bool GLWidget::rayLineParam(const QVector3D& origin, const QVector3D& dir,
                            const QVector3D& center, const QVector3D& axis, float& param) {
    QVector3D D = axis.normalized();
    QVector3D B = dir.normalized();
    float b = QVector3D::dotProduct(D, B);
    if (std::fabs(b) > 0.995f) return false; //正面视角 沿轴无法确定位置
    QVector3D w = origin - center;
    float Aw = QVector3D::dotProduct(D, w);
    float Bw = QVector3D::dotProduct(B, w);
    param = (b * Bw - Aw) / (b * b - 1.0f);
    return true;
    }

//射线与平面求交 交点在相机前方才成功
bool GLWidget::rayPlaneInter(const QVector3D& origin, const QVector3D& dir,
                             const QVector3D& center, const QVector3D& normal, QVector3D& out) {
    float denom = QVector3D::dotProduct(dir, normal);
    if (std::fabs(denom) < 1e-6f) return false;
    float t = QVector3D::dotProduct(center - origin, normal) / denom;
    if (t < 0.0f) return false;
    out = origin + dir * t;
    return true;
    }

//旋转模式 鼠标在圆平面内相对中心的极角(弧度)
float GLWidget::gizmoAngle(const QPoint& pos, int axis) {
    QVector3D n = axis == 1 ? QVector3D(1, 0, 0) : axis == 2 ? QVector3D(0, 1, 0) : QVector3D(0, 0, 1);
    QVector3D origin, dir;
    pickingRay(pos, origin, dir);
    QVector3D inter;
    if (!rayPlaneInter(origin, dir, m_dragCenter, n, inter)) return m_prevDragParam;
    QVector3D d = inter - m_dragCenter;
    if (d.length() < 1e-5f) return m_prevDragParam;
    QVector3D up0 = std::fabs(n.y()) > 0.99f ? QVector3D(1, 0, 0) : QVector3D(0, 1, 0);
    QVector3D e0 = QVector3D::crossProduct(n, up0).normalized();
    QVector3D e1 = QVector3D::crossProduct(n, e0).normalized();
    return std::atan2(QVector3D::dotProduct(d, e1), QVector3D::dotProduct(d, e0));
    }

//命中测试 逐轴将手柄投影到屏幕 取像素距离最近且小于阈值的轴
int GLWidget::gizmoHitAxis(const QPoint& pos) {
    if (m_transformMode == None || !hasModel()) return 0;
    const float s = m_mesh.radius() * 1.2f;
    const QVector3D center = m_mesh.center() + m_transformPos;
    const QVector3D axes[3] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };
    QMatrix4x4 view = m_camera.viewMatrix();
    float aspect = m_fbH > 0 ? (float)m_fbW / m_fbH : 1.0f;
    QMatrix4x4 proj = m_camera.projMatrix(aspect);
    QVector3D origin, dir;
    pickingRay(pos, origin, dir);
    const float threshold = 14.0f; //像素命中半径
    int best = 0;
    float bestDist = threshold;
    for (int i = 0; i < 3; ++i) {
        QVector3D axis = gizmoAxisDir(i + 1);
        QPointF spt;
        if (m_transformMode == Rotate) {
            //圆环 取圆上离射线-平面交点最近点投影
            QVector3D inter;
            if (!rayPlaneInter(origin, dir, center, axis, inter)) continue;
            QVector3D d = inter - center;
            if (d.length() < 1e-5f) continue;
            QVector3D onRing = center + d.normalized() * s;
            spt = projectToScreen(proj, view, onRing);
            }
        else {
            //箭头线段[center, center+axis*s] 取线段上离射线最近点投影
            float param = 0;
            if (!rayLineParam(origin, dir, center, axis, param)) continue;
            param = std::clamp(param, 0.0f, s);
            spt = projectToScreen(proj, view, center + axis * param);
            }
        QPointF dpt = spt - QPointF(pos.x(), pos.y());
        float dist = std::sqrt(dpt.x() * dpt.x() + dpt.y() * dpt.y());
        if (dist < bestDist) {
            bestDist = dist;
            best = i + 1;
            }
        }
    return best;
    }

//按下手柄 初始化该模式拖拽参数
void GLWidget::beginGizmoDrag(const QPoint& pos, int axis) {
    m_dragStartState = currentTransform(); //撤销历史起点
    m_dragCenter = m_mesh.center() + m_transformPos;
    if (m_transformMode == Rotate) {
        m_prevDragParam = gizmoAngle(pos, axis);
        return;
        }
    QVector3D origin, dir;
    pickingRay(pos, origin, dir);
    QVector3D a = gizmoAxisDir(axis);
    if (!rayLineParam(origin, dir, m_dragCenter, a, m_prevDragParam)) m_prevDragParam = 0.0f;
    if (m_transformMode == Scale) {
        if (axis == 1) m_dragInitScale = m_transformScale.x();
        else if (axis == 2) m_dragInitScale = m_transformScale.y();
        else m_dragInitScale = m_transformScale.z();
        }
    }

//移动变换模型 平移=沿轴增量 旋转=极角增量 缩放=沿轴距离比例
void GLWidget::dragGizmo(const QPoint& pos) {
    if (m_dragAxis <= 0 || m_transformMode == None) return;
    QVector3D axis = gizmoAxisDir(m_dragAxis);
    if (m_transformMode == Translate) {
        QVector3D origin, dir;
        pickingRay(pos, origin, dir);
        float param = 0;
        if (!rayLineParam(origin, dir, m_dragCenter, axis, param)) return;
        m_transformPos += axis * (param - m_prevDragParam);
        m_prevDragParam = param;
        }
    else if (m_transformMode == Rotate) {
        float ang = gizmoAngle(pos, m_dragAxis);
        float delta = ang - m_prevDragParam;
        if (delta > kPi) delta -= 2.0f * kPi; //极角跨±π补差
        if (delta < -kPi) delta += 2.0f * kPi;
        m_prevDragParam = ang;
        m_transformRot = QQuaternion::fromAxisAndAngle(axis, delta * 180.0f / kPi) * m_transformRot;
        }
    else if (m_transformMode == Scale) {
        QVector3D origin, dir;
        pickingRay(pos, origin, dir);
        float param = 0;
        if (!rayLineParam(origin, dir, m_dragCenter, axis, param)) return;
        if (std::fabs(m_prevDragParam) < 1e-4f) return;
        float ratio = std::clamp(std::fabs(param) / std::fabs(m_prevDragParam), 0.05f, 20.0f);
        float v = m_dragInitScale * ratio;
        if (m_dragAxis == 1) m_transformScale.setX(v);
        else if (m_dragAxis == 2) m_transformScale.setY(v);
        else m_transformScale.setZ(v);
        }
    updateModelMatrix();
    update();
    }

//轴方向 缩放模式随模型旋转对齐本地轴 平移/旋转保持世界轴向
QVector3D GLWidget::gizmoAxisDir(int idx) const {
    QVector3D a = idx == 1 ? QVector3D(1, 0, 0) : idx == 2 ? QVector3D(0, 1, 0) : QVector3D(0, 0, 1);
    return m_transformMode == Scale ? m_transformRot.rotatedVector(a) : a;
    }

GLWidget::TransformState GLWidget::currentTransform() const {
    return { m_transformPos, m_transformRot, m_transformScale };
    }

//状态近似相等 拖拽后无实际变化则不记历史
bool GLWidget::sameTransform(const TransformState& a, const TransformState& b) const {
    if ((a.pos - b.pos).length() > 1e-5f) return false;
    if (std::fabs(a.scale.x() - b.scale.x()) > 1e-5f ||
        std::fabs(a.scale.y() - b.scale.y()) > 1e-5f ||
        std::fabs(a.scale.z() - b.scale.z()) > 1e-5f) return false;
        const float dr = std::fabs(a.rot.x() - b.rot.x()) + std::fabs(a.rot.y() - b.rot.y()) +
                         std::fabs(a.rot.z() - b.rot.z()) + std::fabs(a.rot.scalar() - b.rot.scalar());
        return dr < 1e-5f;
        }

//套用历史状态 缩放模式手柄随旋转重建
void GLWidget::applyTransform(const TransformState& s) {
    m_transformPos = s.pos;
    m_transformRot = s.rot;
    m_transformScale = s.scale;
    updateModelMatrix();
    if (m_transformMode != None && m_overlayShader && m_overlayShader->isValid() && isValid()) {
        makeCurrent();
        rebuildGizmo();
        doneCurrent();
        }
    update();
    }

void GLWidget::undo() {
    if (m_undoStack.empty()) return;
    m_redoStack.push_back(currentTransform());
    applyTransform(m_undoStack.back());
    m_undoStack.pop_back();
    emit historyChanged(canUndo(), canRedo());
    }

void GLWidget::redo() {
    if (m_redoStack.empty()) return;
    m_undoStack.push_back(currentTransform());
    applyTransform(m_redoStack.back());
    m_redoStack.pop_back();
    emit historyChanged(canUndo(), canRedo());
    }

void GLWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) m_leftDragDist = 0; //左键拖拽距离起点
    if (event->button() == Qt::RightButton) m_rightDragDist = 0;
    m_lastMousePos = event->pos();
    m_dragButton = event->button();
    //左键按下命中操作器手柄 进入变换拖拽 不再旋转相机
    if (event->button() == Qt::LeftButton && m_transformMode != None && hasModel()) {
        int axis = gizmoHitAxis(event->pos());
        if (axis) {
            m_dragAxis = axis;
            beginGizmoDrag(event->pos(), axis);
            return;
            }
        }
    }

void GLWidget::mouseMoveEvent(QMouseEvent* event) {
    QPoint d = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();
    if (m_dragButton == Qt::LeftButton) {
        //操作器拖拽优先 按住手柄移动变换模型
        if (m_dragAxis) {
            dragGizmo(event->pos());
            return;
            }
        m_leftDragDist += qAbs(d.x()) + qAbs(d.y()); //累计 判断是否真拖拽
        m_camera.orbit(d.x() * 0.008f, d.y() * 0.008f);
        }
    else if (m_dragButton == Qt::RightButton || m_dragButton == Qt::MiddleButton) {
        if (m_dragButton == Qt::RightButton)
            m_rightDragDist += qAbs(d.x()) + qAbs(d.y()); //累计 判断是否真拖拽
        m_camera.pan((float)d.x(), (float)d.y());
        }
    else {
        return;
        }
    update();
    }

void GLWidget::mouseReleaseEvent(QMouseEvent* event) {
    //操作器拖拽结束 不触发选中
    if (event->button() == Qt::LeftButton && m_dragAxis) {
        //起点与终点不同 入撤销栈 新操作清空重做栈
        if (!sameTransform(m_dragStartState, currentTransform())) {
            m_undoStack.push_back(m_dragStartState);
            m_redoStack.clear();
            }
        m_dragAxis = 0;
        m_dragButton = Qt::NoButton;
        emit historyChanged(canUndo(), canRedo());
        update();
        return;
        }
    //左键原地点击(未拖拽)才选中 拖拽旋转不改变选中
    if (event->button() == Qt::LeftButton && m_leftDragDist <= 3)
        pickAt(event->pos());
    m_dragButton = Qt::NoButton;
    }

void GLWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (m_rightDragDist > 3) {
        event->accept();    //右键拖拽平移 不弹菜单
        return;
        }
    emit contextMenuRequested(event->globalPos()); //点击 交主窗口弹出开关栏菜单
    event->accept();
    }

void GLWidget::wheelEvent(QWheelEvent* event) {
    m_camera.zoom(event->angleDelta().y() / 120.0f);
    update();
    event->accept();
    }

void GLWidget::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_W:
        case Qt::Key_Up:
            m_camera.panLocal(0, 1, 0);
            break;
        case Qt::Key_S:
        case Qt::Key_Down:
            m_camera.panLocal(0, -1, 0);
            break;
        case Qt::Key_A:
        case Qt::Key_Left:
            m_camera.panLocal(-1, 0, 0);
            break;
        case Qt::Key_D:
        case Qt::Key_Right:
            m_camera.panLocal(1, 0, 0);
            break;
        case Qt::Key_Q:
            m_camera.panLocal(0, 0, 1);
            break;
        case Qt::Key_E:
            m_camera.panLocal(0, 0, -1);
            break;
        case Qt::Key_R:
            resetView();
            return;
        default:
            QOpenGLWidget::keyPressEvent(event);
            return;
        }
    update();
    }

//线段追加
void GLWidget::pushLine(std::vector<OverlayVertex>& out, QVector3D a, QVector3D b, QVector3D color) {
    out.push_back({ a.x(), a.y(), a.z(), color.x(), color.y(), color.z() });
    out.push_back({ b.x(), b.y(), b.z(), color.x(), color.y(), color.z() });
    }

//锥体 箭头头部 底部圆环到尖 8段折线近似
void GLWidget::pushCone(std::vector<OverlayVertex>& out, QVector3D base, QVector3D axis,
                        float len, float radius, QVector3D color) {
    QVector3D d = axis.normalized();
    QVector3D tip = base + d * len;
    QVector3D up = std::fabs(d.y()) > 0.99f ? QVector3D(1, 0, 0) : QVector3D(0, 1, 0);
    QVector3D right = QVector3D::crossProduct(d, up).normalized();
    QVector3D b2 = QVector3D::crossProduct(d, right);
    const int segments = 8;
    for (int i = 0; i < segments; ++i) {
        float a0 = i * 2.0f * kPi / segments;
        float a1 = (i + 1) * 2.0f * kPi / segments;
        QVector3D p0 = base + right * (std::cos(a0) * radius) + b2 * (std::sin(a0) * radius);
        QVector3D p1 = base + right * (std::cos(a1) * radius) + b2 * (std::sin(a1) * radius);
        pushLine(out, p0, tip, color);
        pushLine(out, p0, p1, color);
        }
    }

//圆环 平面法线axis 旋转操作器用
void GLWidget::pushCircle(std::vector<OverlayVertex>& out, QVector3D center, QVector3D axis,
                          float radius, QVector3D color) {
    QVector3D d = axis.normalized();
    QVector3D up = std::fabs(d.y()) > 0.99f ? QVector3D(1, 0, 0) : QVector3D(0, 1, 0);
    QVector3D right = QVector3D::crossProduct(d, up).normalized();
    QVector3D b2 = QVector3D::crossProduct(d, right);
    const int segments = 64;
    QVector3D prev = center + right * radius;
    for (int i = 1; i <= segments; ++i) {
        float a = i * 2.0f * kPi / segments;
        QVector3D p = center + right * (std::cos(a) * radius) + b2 * (std::sin(a) * radius);
        pushLine(out, prev, p, color);
        prev = p;
        }
    }

//线段立方体框 缩放手柄
void GLWidget::pushWireCube(std::vector<OverlayVertex>& out, QVector3D center,
                            float size, QVector3D color) {
    const float h = size * 0.5f;
    QVector3D c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = center + QVector3D((i & 1) ? h : -h, (i & 2) ? h : -h, (i & 4) ? h : -h);
    static const int edges[12][2] = {
            {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}
        };
    for (const auto& edge : edges)
        pushLine(out, c[edge[0]], c[edge[1]], color);
    }

//轴线箭头 轴杆+锥头
void GLWidget::pushAxisArrow(std::vector<OverlayVertex>& out, QVector3D axis,
                             float len, QVector3D color) {
    QVector3D d = axis.normalized();
    QVector3D coneBase = d * (len * 0.72f);
    pushLine(out, QVector3D(), coneBase, color);
    pushCone(out, coneBase, d, len * 0.28f, len * 0.055f, color);
    }

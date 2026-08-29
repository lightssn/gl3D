#include "glwidget.h"
#include "glcontext.h"
#include "shader.h"
#include "meshrenderer.h"
#include "meshloader.h"

#include <QCoreApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <QVector4D>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

static const float kPi = 3.14159265f;

//着色器源码外置为独立文件(exe同目录shaders/) 仿UE引擎.usf加载 改动免重编译
//model.glsl 模型Blinn-Phong头灯 uFlatColor=1纯色(描边/蒙版/拾取) uOutline>0沿法线挤出
//overlay.glsl 线段 网格/坐标轴/操作器共用

//后台模型解析工作对象 常驻加载线程 MeshLoader::load阻塞解析 进度经信号回主线程
//解析(CPU)与渲染上传(需GL context)分离 界面不卡顿
class ModelLoaderWorker : public QObject {
    Q_OBJECT
public slots:
    void load(const QString& path) {
        Mesh mesh;
        QString err;
        const bool ok = MeshLoader::load(path, mesh, &err, [this](int p) { emit progress(p); });
        emit finished(ok, err, mesh);
        }
signals:
    void progress(int percent);            //0~100
    void finished(bool ok, const QString& err, const Mesh& mesh);
};

GLWidget::GLWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setFocusPolicy(Qt::StrongFocus); //接收键盘
    m_fpsTimer.start();
    m_frameTimer.start();
    qRegisterMetaType<DebugSnapshot>("DebugSnapshot");

    //后台加载线程 解析在worker线程 跨线程信号按队列转发
    qRegisterMetaType<Mesh>("Mesh");
    m_loadThread = new QThread(this);
    m_loadWorker = new ModelLoaderWorker();
    m_loadWorker->moveToThread(m_loadThread);
    connect(m_loadWorker, &ModelLoaderWorker::progress, this,
            [this](int p) { emit progressChanged(p); });
    connect(m_loadWorker, &ModelLoaderWorker::finished, this,
            [this](bool ok, const QString& err, const Mesh& mesh) { onLoadFinished(ok, err, mesh); });
    m_loadThread->start();
    }

GLWidget::~GLWidget() {
    //先停后台加载线程 防止析构中回调泄漏
    if (m_loadThread) {
        m_loadThread->quit();
        m_loadThread->wait(); //加载中则等待解析完成
        delete m_loadWorker;
        m_loadWorker = nullptr;
        delete m_loadThread;
        m_loadThread = nullptr;
        }
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
    gl.glEnable(GL_MULTISAMPLE);
    gl.glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], 1.0f);
    m_dpr = devicePixelRatioF();
    printf("[GLWidget] %s\n", (const char*)gl.glGetString(GL_RENDERER));

    m_shader = std::make_unique<Shader>();
    m_overlayShader = std::make_unique<Shader>();
    //着色器源码外置exe同目录shaders/ 运行时加载 仿UE.usf机制
    const QString shaderDir = QCoreApplication::applicationDirPath() + "/shaders/";
    m_shader->loadFromCombinedFile(shaderDir + "model.glsl");
    m_overlayShader->loadFromCombinedFile(shaderDir + "overlay.glsl");
    m_renderer = std::make_unique<MeshRenderer>();

    buildAxesMesh();
    createPickTarget(std::max((int)(width() * m_dpr), 1), std::max((int)(height() * m_dpr), 1));

    //构造函数期间收到过加载请求 此时补传显存
    if (!m_mesh.subMeshes.empty() && m_mesh.vertexCount() > 0) {
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
    m_frameDrawCalls = 0;
    m_depthTest ? gl.glEnable(GL_DEPTH_TEST) : gl.glDisable(GL_DEPTH_TEST);
    m_faceCulling ? gl.glEnable(GL_CULL_FACE) : gl.glDisable(GL_CULL_FACE);
    const qint64 frameElapsed = m_frameTimer.restart();
    if (frameElapsed > 0) {
        m_frameTimesMs.push_back((float)frameElapsed);
        if (m_frameTimesMs.size() > 240) m_frameTimesMs.erase(m_frameTimesMs.begin());
    }
    gl.glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], 1.0f);
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    //FPS统计 每秒发一次
    ++m_frameCount;
    if (m_fpsTimer.elapsed() >= 1000) {
        m_lastFps = m_frameCount;
        emit fpsUpdated(m_lastFps);
        m_frameCount = 0;
        m_fpsTimer.restart();
        }

    if (!m_shader || !m_shader->isValid() || !hasModel()) { m_lastDrawCalls = 0; return; }

    QMatrix4x4 view = m_camera.viewMatrix();
    float aspect = height() > 0 ? (float)width() / height() : 1.0f;
    QMatrix4x4 proj = m_camera.projMatrix(aspect);

    if (m_showGrid) drawGrid(proj, view); //背景网格 先画被模型遮挡

    //主模型 Blinn-Phong头灯
    m_shader->bind();
    m_shader->setMat4("uView", view.constData());
    m_shader->setMat4("uProj", proj.constData());
    m_shader->setFloat("uOutline", 0.0f);
    m_shader->setInt("uFlatColor", 0);
    m_shader->setInt("uDebugView", m_debugView);
    m_shader->setVec4("uFlatColorValue", 1, 1, 1, 1);
    m_renderer->render(*m_shader);
    m_frameDrawCalls += m_renderer->drawUnitCount();

    //线框模式 复用已上传VAO 单片状态切换追加三角边 无CPU几何开销
    //深度LEQUAL保证边与已被填充的前表面同深度通过 只描可见三角
    if (m_wireframe) {
        //根据背景亮暗选线框颜色
        float lum = (m_clearColor[0] + m_clearColor[1] + m_clearColor[2]) / 3.0f;
        QVector3D wc = lum > 0.5f ? QVector3D(0.30f, 0.30f, 0.34f) : QVector3D(0.74f, 0.74f, 0.78f);
        m_shader->setFloat("uOutline", 0.0f);
        m_shader->setInt("uFlatColor", 1); //纯色模式
        m_shader->setVec4("uFlatColorValue", wc.x(), wc.y(), wc.z(), 1.0f);
        gl.glDepthFunc(GL_LEQUAL); //线框与模型同一深度
        gl.glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); //切换线框模式
        gl.glLineWidth(1.0f);
        m_renderer->render(*m_shader);
        m_frameDrawCalls += m_renderer->drawUnitCount();
        gl.glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); //切回
        gl.glDepthFunc(GL_LESS); //防闪烁
        }

    if (m_selectedSubMesh >= 0 && m_showSelection) {
        drawSelection(proj, view); //红边+粉色蒙版
        m_frameDrawCalls += m_renderer->drawUnitCount() * 3;
    }
    m_shader->unbind();

    //操作器与坐标轴HUD 顶层显示 关深度测试
    gl.glDisable(GL_DEPTH_TEST);
    if (m_transformMode != None && m_showGizmo) drawGizmo(proj, view);
    if (m_showAxes) drawAxesHud(view);
    m_depthTest ? gl.glEnable(GL_DEPTH_TEST) : gl.glDisable(GL_DEPTH_TEST);
    m_lastDrawCalls = m_frameDrawCalls;
    if (m_frameCount % 8 == 0) emit debugUpdated(collectDebugSnapshot());
    }//paintGL

static qint64 textureBytes(GLFunctions& gl, unsigned int id) {
    gl.glBindTexture(GL_TEXTURE_2D, id);
    GLint width = 0, height = 0, levels = 1, format = GL_RGBA8, minFilter = GL_LINEAR;
    gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
    gl.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
    gl.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS, &levels);
    if (levels <= 0) {
        levels = 1;
        if (minFilter == GL_NEAREST_MIPMAP_NEAREST || minFilter == GL_LINEAR_MIPMAP_NEAREST ||
            minFilter == GL_NEAREST_MIPMAP_LINEAR || minFilter == GL_LINEAR_MIPMAP_LINEAR) {
            levels = 1; for (int d = std::max(width, height); d > 1; d >>= 1) ++levels;
        }
    }
    int bytesPerPixel = (format == GL_R8 ? 1 : format == GL_RG8 ? 2 : format == GL_RGB8 ? 3 : 4);
    qint64 total = 0;
    for (int level = 0; level < levels; ++level)
        total += (qint64)std::max(1, width >> level) * std::max(1, height >> level) * bytesPerPixel;
    return total;
}

DebugSnapshot GLWidget::collectDebugSnapshot() {
    DebugSnapshot s;
    s.fps = m_lastFps;
    s.drawCalls = m_lastDrawCalls;
    s.triangles = m_mesh.triangleCount();
    s.frameTimesMs.reserve(static_cast<qsizetype>(m_frameTimesMs.size()));
    for (float frameTime : m_frameTimesMs)
        s.frameTimesMs.append(frameTime);
    if (!m_frameTimesMs.empty()) {
        s.averageFrameMs = std::accumulate(m_frameTimesMs.begin(), m_frameTimesMs.end(), 0.0f) / m_frameTimesMs.size();
        s.lowFrameMs = *std::max_element(m_frameTimesMs.begin(), m_frameTimesMs.end());
        std::vector<float> sorted = m_frameTimesMs;
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&sorted](float p) { return sorted[std::min(static_cast<size_t>(p * (sorted.size() - 1)), sorted.size() - 1)]; };
        s.p95FrameMs = percentile(0.95f);
        s.p99FrameMs = percentile(0.99f);
        const size_t slowStart = static_cast<size_t>(0.99f * sorted.size());
        const float slowSum = std::accumulate(sorted.begin() + std::min(slowStart, sorted.size() - 1), sorted.end(), 0.0f);
        const size_t slowCount = sorted.size() - std::min(slowStart, sorted.size() - 1);
        const float slowAverage = slowSum / slowCount;
        s.onePercentLowFps = slowAverage > 0.0f ? 1000.0f / slowAverage : 0.0f;
    }
    s.fov = m_camera.fov(); s.distance = m_camera.distance();
    s.eye = m_camera.eye(); s.target = m_camera.target(); s.ortho = m_camera.ortho();
    s.antialiasing = m_antialiasing; s.modelLoaded = hasModel();
    if (!isValid()) return s;
    auto& gl = GLFunctions::instance();
    const auto* renderer = gl.glGetString(GL_RENDERER);
    const auto* version = gl.glGetString(GL_VERSION);
    s.renderer = renderer ? QString::fromLatin1(reinterpret_cast<const char*>(renderer)) : QString();
    s.glVersion = version ? QString::fromLatin1(reinterpret_cast<const char*>(version)) : QString();
    std::vector<unsigned int> vaos, vbos, ebos, textures;
    if (m_renderer) m_renderer->collectResourceIds(vaos, vbos, ebos, textures);
    if (m_gridMesh.vao) vaos.push_back(m_gridMesh.vao), vbos.push_back(m_gridMesh.vbo);
    if (m_axesMesh.vao) vaos.push_back(m_axesMesh.vao), vbos.push_back(m_axesMesh.vbo);
    if (m_gizmoMesh.vao) vaos.push_back(m_gizmoMesh.vao), vbos.push_back(m_gizmoMesh.vbo);
    qint64 bufferBytes = 0;
    for (unsigned int id : vbos) { GLint n = 0; gl.glBindBuffer(GL_ARRAY_BUFFER, id); gl.glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &n); bufferBytes += n; }
    qint64 eboBytes = 0;
    for (unsigned int id : ebos) { GLint n = 0; gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, id); gl.glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &n); eboBytes += n; }
    qint64 textureMemory = 0; for (unsigned int id : textures) textureMemory += textureBytes(gl, id);
    qint64 pickTextureBytes = 0;
    if (m_pickColor) { pickTextureBytes = textureBytes(gl, m_pickColor); textures.push_back(m_pickColor); textureMemory += pickTextureBytes; }
    GLint rw = 0, rh = 0, rfmt = 0;
    if (m_pickDepth) { gl.glBindRenderbuffer(GL_RENDERBUFFER, m_pickDepth); gl.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &rw); gl.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &rh); gl.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &rfmt); }
    const qint64 depthBytes = (qint64)rw * rh * (rfmt == GL_DEPTH_COMPONENT16 ? 2 : 4);
    qint64 cpuVboBytes = 0, cpuEboBytes = 0;
    for (const auto& subMesh : m_mesh.subMeshes) {
        cpuVboBytes += static_cast<qint64>(subMesh.vertices.size() * sizeof(Vertex));
        cpuEboBytes += static_cast<qint64>(subMesh.indices.size() * sizeof(unsigned int));
    }
    const qint64 cpuTextureBytes = m_renderer ? m_renderer->textureCpuBytes() : 0;
    s.resources = {{"VAO", (int)vaos.size(), 0, 0, false}, {"VBO", (int)vbos.size(), cpuVboBytes, bufferBytes, true}, {"EBO", (int)ebos.size(), cpuEboBytes, eboBytes, true}, {"纹理", (int)textures.size(), cpuTextureBytes, textureMemory, true}, {"FBO", m_pickFbo ? 1 : 0, 0, pickTextureBytes + depthBytes, true}};
    return s;
}

DebugSnapshot GLWidget::debugSnapshot() {
    makeCurrent();
    DebugSnapshot s = collectDebugSnapshot();
    doneCurrent();
    return s;
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
    ++m_frameDrawCalls;
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
    m_shader->setInt("uFlatColor", 1);

    //1 选中模型写入stencil 颜色忽略 深度用LEQUAL同深度通过
    gl.glEnable(GL_STENCIL_TEST);
    gl.glStencilFunc(GL_ALWAYS, 1, 0xFF);
    gl.glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    gl.glStencilMask(0xFF);
    gl.glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    gl.glDepthFunc(GL_LEQUAL);
    m_shader->setFloat("uOutline", 0.0f);
    m_renderer->render(*m_shader, m_selectedSubMesh);
    gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    //2 沿法线膨胀画红边 只画stencil未覆盖的外缘
    //保持深度测试 膨胀外缘比原表面更近 通过 而模型前方网格已写入更近深度 遮挡红边
    m_shader->setFloat("uOutline", m_outlineWidth);
    m_shader->setVec4("uFlatColorValue", 0.9f, 0.1f, 0.1f, 1.0f);
    gl.glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    gl.glStencilMask(0x00);
    gl.glDepthFunc(GL_LEQUAL);
    m_renderer->render(*m_shader, m_selectedSubMesh);
    gl.glStencilMask(0xFF);
    gl.glDisable(GL_STENCIL_TEST);

    //3 浅粉色半透明蒙版 LEQUAL避免同深度被剔除
    gl.glEnable(GL_BLEND);
    gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_shader->setFloat("uOutline", 0.0f);
    m_shader->setVec4("uFlatColorValue", 1.0f, 0.76f, 0.82f, 0.35f);
    gl.glDepthMask(GL_FALSE);
    gl.glDepthFunc(GL_LEQUAL);
    m_renderer->render(*m_shader, m_selectedSubMesh);
    gl.glDepthFunc(GL_LESS);
    gl.glDepthMask(GL_TRUE);
    gl.glDisable(GL_BLEND);
    }

void GLWidget::drawGizmo(const QMatrix4x4& proj, const QMatrix4x4& view) {
    QMatrix4x4 mvp = proj * view;
    const QVector3D center = m_selectedSubMesh >= 0 ? m_subMeshCenters[static_cast<size_t>(m_selectedSubMesh)] : m_mesh.center();
    mvp.translate(center + m_transformPos); //操作器定位在选中子网格中央
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
        m_shader->setFloat("uOutline", 0.0f);
        m_shader->setInt("uFlatColor", 1);
        m_shader->setVec4("uFlatColorValue", 1, 0, 0, 1); //命中=红
        for (int i = 0; i < subMeshCount(); ++i) {
            if (!m_renderer->subMeshVisible(i)) continue;
            const int id = i + 1;
            m_shader->setVec4("uFlatColorValue", (id & 255) / 255.0f,
                              ((id >> 8) & 255) / 255.0f,
                              ((id >> 16) & 255) / 255.0f, 1.0f);
            m_renderer->render(*m_shader, i);
        }
        m_shader->unbind();

        unsigned char pixel[4] = { 0, 0, 0, 0 };
        gl.glReadPixels(x, m_pickH - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel); //GL y轴向上
        const int picked = static_cast<int>(pixel[0]) |
                           (static_cast<int>(pixel[1]) << 8) |
                           (static_cast<int>(pixel[2]) << 16);
        hit = picked > 0;
        if (hit) selectSubMesh(picked - 1);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        gl.glViewport(0, 0, m_fbW, m_fbH);
        }
    doneCurrent();
    if (!hit && m_selectedSubMesh >= 0) selectSubMesh(-1);
    update();
    }

bool GLWidget::hasModel() const {
    return m_renderer && !m_renderer->empty();
    }

bool GLWidget::loadModel(const QString& path) {
    if (m_loading) return false; //加载进行中 忽略重复请求
    m_loading = true;
    m_currentPath = path;
    emit loadStarted(); //主窗口显示进度条
    //解析交后台线程 完成回调onLoadFinished 不做GPU上传与取景
    QMetaObject::invokeMethod(m_loadWorker, "load", Qt::QueuedConnection,
                              Q_ARG(QString, path));
    return true;
    }

//后台解析完成回调(主线程) 成功则上传显存+取景+发modelLoaded 失败发loadFailed
void GLWidget::onLoadFinished(bool ok, const QString& err, const Mesh& mesh) {
    m_loading = false;
    if (!ok) {
        emit loadFailed(err.isEmpty() ? "加载失败" : err);
        emit loadFinished();
        update();
        return;
        }
    m_mesh = mesh;
    m_subMeshCenters.clear();
    m_subMeshCenters.reserve(m_mesh.subMeshes.size());
    for (const auto& subMesh : m_mesh.subMeshes) {
        if (subMesh.vertices.empty()) {
            m_subMeshCenters.push_back(m_mesh.center());
            continue;
        }
        QVector3D minPos(subMesh.vertices.front().px, subMesh.vertices.front().py, subMesh.vertices.front().pz);
        QVector3D maxPos = minPos;
        for (const auto& vertex : subMesh.vertices) {
            minPos.setX(std::min(minPos.x(), vertex.px));
            minPos.setY(std::min(minPos.y(), vertex.py));
            minPos.setZ(std::min(minPos.z(), vertex.pz));
            maxPos.setX(std::max(maxPos.x(), vertex.px));
            maxPos.setY(std::max(maxPos.y(), vertex.py));
            maxPos.setZ(std::max(maxPos.z(), vertex.pz));
        }
        m_subMeshCenters.push_back((minPos + maxPos) * 0.5f);
    }
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
    m_selectedSubMesh = -1;
    emit selectionChanged(false); //新模型默认未选中 操作器按钮隐藏

    QString info = QString("%1  顶点%2  三角%3  子网格%4")
                   .arg(QFileInfo(m_currentPath).fileName())
                   .arg(m_mesh.vertexCount())
                   .arg(m_mesh.triangleCount())
                   .arg((int)m_mesh.subMeshes.size());
    emit modelLoaded(info);
    m_mesh.releaseGeometry();
    emit loadFinished();
    update();
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
    m_subMeshCenters.clear();
    m_transformPos = QVector3D();
    m_transformRot = QQuaternion();
    m_transformScale = QVector3D(1, 1, 1);
    m_dragAxis = 0;
    m_transformMode = None;
    m_undoStack.clear();
    m_redoStack.clear();
    emit historyChanged(false, false);
    m_selectedSubMesh = -1;
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

void GLWidget::setWireframe(bool on) {
    m_wireframe = on;
    update();
    }

void GLWidget::setOutlineWidth(float width) {
    m_outlineWidth = std::max(0.0f, width);
    update();
    }

int GLWidget::subMeshCount() const {
    return static_cast<int>(m_mesh.subMeshes.size());
    }

QString GLWidget::subMeshName(int index) const {
    if (index < 0 || index >= subMeshCount()) return QString();
    const std::string& name = m_mesh.subMeshes[static_cast<size_t>(index)].materialName;
    return name.empty() ? QStringLiteral("(默认材质)") : QString::fromStdString(name);
    }

bool GLWidget::subMeshVisible(int index) const {
    return m_renderer && m_renderer->subMeshVisible(index);
    }

void GLWidget::setSubMeshVisible(int index, bool visible) {
    if (!m_renderer) return;
    m_renderer->setSubMeshVisible(index, visible);
    update();
    }

void GLWidget::selectSubMesh(int index) {
    if (index < -1 || index >= subMeshCount()) return;
    if (index == m_selectedSubMesh) {
        emit subMeshSelected(index);
        return;
    }
    m_selectedSubMesh = index;
    m_transformPos = QVector3D();
    m_transformRot = QQuaternion();
    m_transformScale = QVector3D(1, 1, 1);
    m_undoStack.clear();
    m_redoStack.clear();
    updateModelMatrix();
    emit historyChanged(false, false);
    emit selectionChanged(m_selectedSubMesh >= 0);
    emit subMeshSelected(m_selectedSubMesh);
    update();
    }

void GLWidget::setAntialiasing(bool on) {
    m_antialiasing = on;
    if (isValid()) { makeCurrent(); on ? GLFunctions::instance().glEnable(GL_MULTISAMPLE) : GLFunctions::instance().glDisable(GL_MULTISAMPLE); doneCurrent(); }
    update();
    }

void GLWidget::setMipmaps(bool on) {
    if (m_renderer) {
        makeCurrent();
        m_renderer->setMipmapsEnabled(on);
        doneCurrent();
    }
    update();
}

void GLWidget::setShowGrid(bool on) { m_showGrid = on; update(); }
void GLWidget::setShowGizmo(bool on) { m_showGizmo = on; update(); }
void GLWidget::setShowSelection(bool on) { m_showSelection = on; update(); }
void GLWidget::setShowAxes(bool on) { m_showAxes = on; update(); }
void GLWidget::setDepthTest(bool on) { m_depthTest = on; update(); }
void GLWidget::setFaceCulling(bool on) { m_faceCulling = on; update(); }
void GLWidget::setDebugView(int mode) { m_debugView = std::clamp(mode, 0, 2); update(); }

//组合模型矩阵 T(c+pos)*R*S*T(-c) 绕模型中心缩放旋转 中心跟随位移
void GLWidget::updateModelMatrix() {
    QMatrix4x4 m;
    QVector3D c = m_selectedSubMesh >= 0 && m_selectedSubMesh < static_cast<int>(m_subMeshCenters.size())
        ? m_subMeshCenters[static_cast<size_t>(m_selectedSubMesh)] : m_mesh.center();
    m.translate(c + m_transformPos);
    m.rotate(m_transformRot);
    m.scale(m_transformScale);
    m.translate(-c);
    m_modelMatrix = m;
    if (m_renderer) {
        QMatrix4x4 identity;
        identity.setToIdentity();
        for (int i = 0; i < subMeshCount(); ++i)
            m_renderer->setSubMeshTransform(i, identity);
        if (m_selectedSubMesh >= 0)
            m_renderer->setSubMeshTransform(m_selectedSubMesh, m_modelMatrix);
    }
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
    const QVector3D center = (m_selectedSubMesh >= 0 ? m_subMeshCenters[static_cast<size_t>(m_selectedSubMesh)] : m_mesh.center()) + m_transformPos;
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
    m_dragCenter = (m_selectedSubMesh >= 0 ? m_subMeshCenters[static_cast<size_t>(m_selectedSubMesh)] : m_mesh.center()) + m_transformPos;
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

#include "glwidget.moc" //AUTOMOC: cpp内Q_OBJECT的ModelLoaderWorker元信息

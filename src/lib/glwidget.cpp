#include "glwidget.h"
#include "glcontext.h"
#include "shader.h"
#include "meshrenderer.h"
#include "meshloader.h"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QFileInfo>
#include <cstdio>

//Blinn-Phong头灯模型 光源随相机移动 任意角度无死角
static const char* VERT_SRC = R"(
#version 330 core
//输入: 位置(location=0)、纹理坐标(1)、法线(2)
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

//输出到片段着色器
out vec3 vNormal; //世界空间法线
out vec2 vUV; //纹理坐标
uniform mat4 uView; //观察矩阵: 世界 → 观察
uniform mat4 uProj; //投影矩阵: 观察 → 裁剪
void main() {
    gl_Position = uProj * uView * vec4(aPos, 1.0);
    vNormal = mat3(transpose(inverse(uView))) * aNormal; //法线随视图变换 光照在观察空间算
    vUV = aUV;
}
)";

//片段着色器，计算像素颜色：纹理采样 × 光照
static const char* FRAG_SRC = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor; //像素颜色
uniform sampler2D uTexture;
uniform int uUseTexture;
uniform vec3 uDiffuse;
void main() {
    vec3 base = uUseTexture == 1 ? texture(uTexture, vUV).rgb : uDiffuse; //底色（纹理/纯色）
    vec3 n = normalize(vNormal); //插值后法线≠1，重归一化
    //观察空间头灯 光线沿视线方向 法线背向时翻转 兼容单面片
    vec3 light = vec3(0.0, 0.0, 1.0); //头灯光源（观察空间坐标，z=1同视线）
    float diff = abs(dot(n, light)) * 0.75; //漫反射，不用max以兼容单面片
    vec3 h = normalize(light + vec3(0.0, 0.0, 1.0)); //半程向量
    float spec = pow(max(abs(dot(n, h)), 0.0), 32.0) * 0.25;
    //高光强度，模拟亮斑。锐度32（值越大光斑越小）强度0.25
    vec3 color = base * (0.30 + diff) + vec3(spec);
    FragColor = vec4(color, 1.0); //alpha=1
}
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
    doneCurrent();
    GLFunctions::destroy();
    }

void GLWidget::initializeGL() {
    GLFunctions::init();
    auto& gl = GLFunctions::instance();
    gl.glEnable(GL_DEPTH_TEST);
    gl.glDisable(GL_CULL_FACE); //叶片等薄片需双面可见
    gl.glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], 1.0f);
    printf("[GLWidget] %s\n", (const char*)gl.glGetString(GL_RENDERER));

    m_shader = std::make_unique<Shader>();
    m_shader->loadFromSource(VERT_SRC, FRAG_SRC);
    m_renderer = std::make_unique<MeshRenderer>();

    //构造函数期间收到过加载请求 此时补传显存
    if (!m_mesh.subMeshes.empty())
        m_renderer->upload(m_mesh);
    }

void GLWidget::resizeGL(int w, int h) {
    GLFunctions::instance().glViewport(0, 0, w, h);
    }

void GLWidget::paintGL() {
    auto& gl = GLFunctions::instance();
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    //FPS统计 每秒发一次
    ++m_frameCount;
    if (m_fpsTimer.elapsed() >= 1000) {
        emit fpsUpdated(m_frameCount);
        m_frameCount = 0;
        m_fpsTimer.restart();
        }

    if (!m_shader || !m_shader->isValid() || !hasModel()) return;

    m_shader->bind();
    float aspect = height() > 0 ? (float)width() / height() : 1.0f;
    QMatrix4x4 view = m_camera.viewMatrix();
    m_shader->setMat4("uView", view.constData());
    m_shader->setMat4("uProj", m_camera.projMatrix(aspect).constData());
    m_renderer->render(*m_shader);
    m_shader->unbind();
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
        doneCurrent();
        }
    m_camera.fitToSphere(m_mesh.center(), m_mesh.radius());

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
    GLFunctions::instance().glClearColor(r, g, b, 1.0f);
    doneCurrent();
    update();
    }

void GLWidget::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    m_dragButton = event->button();
    }

void GLWidget::mouseMoveEvent(QMouseEvent* event) {
    QPoint d = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();
    if (m_dragButton == Qt::LeftButton) {
        m_camera.orbit(d.x() * 0.008f, d.y() * 0.008f);
        }
    else if (m_dragButton == Qt::RightButton || m_dragButton == Qt::MiddleButton) {
        m_camera.pan((float)d.x(), (float)d.y());
        }
    else {
        return;
        }
    update();
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

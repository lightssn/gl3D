#include "glcontext.h"
#include "meshrenderer.h"
#include "renderscene.h"
#include "shader.h"
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QThread>

// 离屏验证增量上传、共享纹理、整瓦片筛选和资源释放。
int main(int argc, char** argv) {
    QGuiApplication app(argc,argv);
    QSurfaceFormat format; format.setVersion(4,5); format.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext context; context.setFormat(format);
    QOffscreenSurface surface; surface.setFormat(format); surface.create();
    if(!context.create() || !context.makeCurrent(&surface) || !GLFunctions::init()) {
        qCritical()<<"Cannot create OpenGL 4.5 offscreen context"; return 1;
    }
    int result=0;
    {
        // 编译实际模型着色器，覆盖像素描边的GLSL和uniform变更。
        Shader modelShader;
        const QString shaderPath = QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath("../src/app/shaders/model.glsl");
        if (!modelShader.loadFromCombinedFile(shaderPath)) return 1;
        auto& gl=GLFunctions::instance();
        {
            // 实际光栅化比较外扩前后的位置，覆盖透视、正交和非均匀缩放。
            Mesh outlineMesh;
            SubMesh triangle;
            triangle.vertices={{-0.4f,-0.4f,0,1,0,0,0,0},{0.4f,-0.4f,0,1,0,0,0,0},{0,0.4f,0,1,0,0,0,0}};
            triangle.indices={0,1,2}; outlineMesh.subMeshes.push_back(std::move(triangle));
            MeshRenderer outlineRenderer; outlineRenderer.upload(outlineMesh);
            QOpenGLFramebufferObject outlineFbo(128,128); outlineFbo.bind();
            gl.glViewport(0,0,128,128); gl.glClearColor(0,0,0,1);
            modelShader.bind();
            modelShader.setVec3("uViewport",128,128,0);
            modelShader.setInt("uFlatColor",1); modelShader.setVec4("uFlatColorValue",1,0,0,1);
            for (bool perspective : {false,true}) for (float scale : {0.5f,1.0f,2.0f}) {
                QMatrix4x4 model,view,projection;
                model.scale(scale,scale*0.7f,scale*1.3f);
                view.translate(0,0,-(scale+2.0f));
                if(perspective) projection.perspective(60,1,0.1f,100);
                else projection.ortho(-2,2,-2,2,0.1f,100);
                outlineRenderer.setSubMeshTransform(0,model);
                modelShader.setMat4("uView",view.constData()); modelShader.setMat4("uProj",projection.constData());
                auto leftEdge = [&](float width) {
                    gl.glClear(GL_COLOR_BUFFER_BIT); modelShader.setFloat("uOutline",width);
                    outlineRenderer.render(modelShader);
                    const QImage pixels=outlineFbo.toImage();
                    int edge=128;
                    for(int y=0;y<128;++y) for(int x=0;x<edge;++x)
                        if(pixels.pixelColor(x,y).red()>200) { edge=x; break; }
                    return edge;
                };
                const int original=leftEdge(0), expanded=leftEdge(4);
                if(original==128 || expanded-original!=4) {
                    qCritical()<<"Pixel outline changed with projection/scale"<<perspective<<scale<<original<<expanded;
                    result=9;
                }
            }
            modelShader.unbind(); outlineFbo.release();
        }
        Mesh mesh; mesh.bboxMin={-1,-1,-1}; mesh.bboxMax={1,1,1};
        mesh.tileRoot=std::make_shared<TileNode>(); mesh.tileRoot->owner=0; mesh.tileRoot->radius=2;
        QImage texture(2,2,QImage::Format_RGBA8888); texture.fill(Qt::red);
        for(int side=0;side<2;++side) {
            auto node=std::make_shared<TileNode>(); node->owner=0; node->radius=1;
            SubMesh part; part.baseImage=texture; part.flipTextureVertically=false;
            const float x=side?0.5f:-0.5f;
            part.vertices={{x-0.4f,-0.7f,0,0,0,1,0,0},{x+0.4f,-0.7f,0,0,0,1,1,0},{x,0.7f,0,0,0,1,0.5f,1}};
            part.vertices.resize(40000,part.vertices[0]); part.indices={0,1,2};
            node->geometry.push_back(std::move(part)); mesh.tileRoot->children.push_back(node);
        }
        RenderScene scene; scene.setMesh(std::move(mesh)); MeshRenderer renderer;
        for(int frame=0;frame<120;++frame) {
            scene.updateTiles({64,64}); renderer.syncTiles(scene);
            if(renderer.visibleDrawCount()==2) break;
            QThread::msleep(1);
        }
        std::vector<unsigned> vaos,vbos,ebos,textures;
        renderer.collectResourceIds(vaos,vbos,ebos,textures);
        if(renderer.visibleDrawCount()!=2 || textures.size()!=1 || !scene.mesh().subMeshes[0].vertices.empty()) {
            qCritical()<<"Incremental upload/shared texture failed"<<renderer.visibleDrawCount()<<textures.size(); result=2;
        }
        Shader shader;
        if(!shader.loadFromSource(
            "#version 450 core\nlayout(location=0) in vec3 p; uniform mat4 uModel; void main(){gl_Position=uModel*vec4(p,1);}",
            "#version 450 core\nuniform sampler2D uTexture; uniform int uUseTexture; uniform vec3 uDiffuse; uniform vec4 uTileTint; out vec4 c; void main(){vec3 base=uUseTexture!=0?texture(uTexture,vec2(0.5)).rgb:uDiffuse; c=vec4(mix(base,uTileTint.rgb,uTileTint.a),1);}")) result=3;
        QOpenGLFramebufferObject fbo(64,64);
        fbo.bind(); gl.glViewport(0,0,64,64); gl.glClearColor(0,0,0,1); gl.glClear(GL_COLOR_BUFFER_BIT);
        shader.bind(); renderer.render(shader,0); shader.unbind();
        const QImage pixels=fbo.toImage();
        if(pixels.pixelColor(16,32).red()<200 || pixels.pixelColor(48,32).red()<200) {
            qCritical()<<"Tile material group/texture rendering failed"; result=4;
        }
        scene.mesh().subMeshes[1].tileLevel=5;
        scene.options().tileLevelTint=true; renderer.syncTiles(scene);
        gl.glClear(GL_COLOR_BUFFER_BIT); shader.bind(); renderer.render(shader); shader.unbind();
        const QImage tinted=fbo.toImage();
        if(tinted.pixelColor(16,32).green()<100 ||
           tinted.pixelColor(16,32).green()-tinted.pixelColor(48,32).green()<30) {
            qCritical()<<"Shared L0/L5 tint uniforms failed"; result=7;
        }
        scene.options().tileLevelTint=false; renderer.syncTiles(scene);
        gl.glClear(GL_COLOR_BUFFER_BIT); shader.bind(); renderer.render(shader); shader.unbind();
        if(fbo.toImage().pixelColor(16,32).green()>5) { qCritical()<<"Tint toggle did not restore texture"; result=8; }
        renderer.setMipmapsEnabled(true);
        // 实际像素验证两种光照模式关闭法线光照后只剩环境项。
        modelShader.bind();
        QMatrix4x4 identity;
        modelShader.setMat4("uView",identity.constData()); modelShader.setMat4("uProj",identity.constData());
        modelShader.setInt("uFlatColor",0); modelShader.setInt("uDebugView",0); modelShader.setFloat("uOutline",0);
        for(int pbr : {0,1}) {
            modelShader.setInt("uPbr",pbr);
            modelShader.setInt("uNormalLighting",1);
            gl.glClear(GL_COLOR_BUFFER_BIT); renderer.render(modelShader);
            const int lit=fbo.toImage().pixelColor(16,32).red();
            modelShader.setInt("uNormalLighting",0);
            gl.glClear(GL_COLOR_BUFFER_BIT); renderer.render(modelShader);
            const int ambient=fbo.toImage().pixelColor(16,32).red();
            if(lit<=ambient+20 || ambient<1 || ambient>80) {
                qCritical()<<"Normal lighting toggle failed"<<pbr<<lit<<ambient; result=10;
            }
        }
        modelShader.unbind();
        renderer.setMipmapsEnabled(false);
        renderer.clear();
        if(gl.glIsTexture(textures.empty()?0:textures[0])) { qCritical()<<"Texture leaked"; result=5; }
        const auto error=gl.glGetError(); if(error!=GL_NO_ERROR) { qCritical()<<"GL error"<<error; result=6; }
        fbo.release();
    }
    GLFunctions::destroy(); context.doneCurrent();
    if(!result) qInfo()<<"OpenGL tiled renderer smoke test passed";
    return result;
}

//================ 顶点着色器 VERTEX ================
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

//================ 片段着色器 FRAGMENT ================
#version 460 core
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTexture; //纹理，glGenTextures / glBindTexture / glTexImage2D创建
uniform int uUseTexture; //纹理开关
uniform vec3 uDiffuse; //纯色底色
uniform int uFlatColor; //纯色开关
uniform vec4 uFlatColorValue; //含alpha纯色值
uniform int uDebugView; //0光照 1法线 2UV

void main() {
    //选中红边/粉色蒙版，跳过光照
    if (uFlatColor == 1) { FragColor = uFlatColorValue; return; }

    vec3 n = normalize(vNormal); //插值后法线≠1 重归一化
    if (uDebugView == 1) { FragColor = vec4(n * 0.5 + 0.5, 1.0); return; }
    if (uDebugView == 2) { FragColor = vec4(fract(vUV), 0.0, 1.0); return; }
    vec3 base = uUseTexture == 1 ? texture(uTexture, vUV).rgb : uDiffuse; //底色=纹理rgb或纯色
    vec3 light = vec3(0.0, 0.0, 1.0); //头灯，视线z
    float diff = abs(dot(n, light)) * 0.75; //漫反射，dot(n, light)为法线和光线夹角余弦值，-1背黑~0侧中~1正亮，abs兼容单面片
    vec3 h = normalize(light + vec3(0.0, 0.0, 1.0)); //半程向量
    float spec = pow(max(abs(dot(n, h)), 0.0), 32.0) * 0.25; //高光锐度32，压制高光0.25
    vec3 color = base * (0.30 + diff) + vec3(spec); //环境光0.3
    FragColor = vec4(color, 1.0);
}

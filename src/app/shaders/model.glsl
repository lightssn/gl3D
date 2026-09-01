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
out vec3 vPosition; //观察空间位置，用于PBR视线方向和切线空间

void main() {
    vec4 wp = uModel * vec4(aPos, 1.0); //模型顶点→世界顶点
    vec3 wn = mat3(uModel) * aNormal; //世界法线，mat3去缩放
    if (uOutline > 0.0) wp.xyz += wn * uOutline; //顶点沿法线外推，生成选中外轮廓
    gl_Position = uProj * uView * wp; //glsl规范不写out
    vNormal = mat3(transpose(inverse(uView))) * wn; //观察法线=视图矩阵inverse求逆去缩放+transpose转置确保垂直+mat3去平移*世界法线
    vUV = aUV; //光栅化插值
    vPosition = (uView * wp).xyz;
}

//================ 片段着色器 FRAGMENT ================
#version 460 core
in vec3 vNormal;
in vec2 vUV;
in vec3 vPosition;
out vec4 FragColor;

uniform sampler2D uTexture; //纹理，glGenTextures / glBindTexture / glTexImage2D创建
uniform int uUseTexture; //纹理开关
uniform vec3 uDiffuse; //纯色底色
uniform int uFlatColor; //纯色开关
uniform vec4 uFlatColorValue; //含alpha纯色值
uniform int uDebugView; //0光照 1法线 2UV
uniform int uPbr; //0保持旧版Blinn-Phong，1启用Cook-Torrance PBR
uniform sampler2D uMetallicRoughnessTexture; //glTF ORM：G粗糙度 B金属度
uniform sampler2D uNormalTexture;
uniform int uUseMetallicRoughness;
uniform int uUseNormalMap;
uniform float uMetallic;
uniform float uRoughness;

float srgbToLinear(float value) {
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

vec3 srgbToLinear(vec3 value) {
    return vec3(srgbToLinear(value.r), srgbToLinear(value.g), srgbToLinear(value.b));
}

vec3 linearToSrgb(vec3 value) {
    value = max(value, vec3(0.0));
    return mix(value * 12.92, 1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055,
               step(vec3(0.0031308), value));
}

float distributionGGX(vec3 n, vec3 h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float nDotH = max(dot(n, h), 0.0);
    float nDotH2 = nDotH * nDotH;
    float denominator = nDotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * denominator * denominator, 0.0001);
}

float geometrySchlickGGX(float nDotV, float roughness) {
    float r = roughness + 1.0;
    float k = r * r / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

float geometrySmith(vec3 n, vec3 v, vec3 l, float roughness) {
    return geometrySchlickGGX(max(dot(n, v), 0.0), roughness) *
           geometrySchlickGGX(max(dot(n, l), 0.0), roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    //选中红边/粉色蒙版，跳过光照
    if (uFlatColor == 1) { FragColor = uFlatColorValue; return; }

    vec3 n = normalize(vNormal); //插值后法线≠1 重归一化
    if (uDebugView == 1) { FragColor = vec4(n * 0.5 + 0.5, 1.0); return; }
    if (uDebugView == 2) { FragColor = vec4(fract(vUV), 0.0, 1.0); return; }
    vec3 base = uUseTexture == 1 ? texture(uTexture, vUV).rgb : uDiffuse; //底色=纹理rgb或纯色
    if (uPbr == 0) {
        vec3 light = vec3(0.0, 0.0, 1.0); //头灯，视线z
        float diff = abs(dot(n, light)) * 0.75; //旧版漫反射
        vec3 h = normalize(light + vec3(0.0, 0.0, 1.0));
        float spec = pow(max(abs(dot(n, h)), 0.0), 32.0) * 0.25;
        FragColor = vec4(base * (0.30 + diff) + vec3(spec), 1.0);
        return;
    }

    if (uUseTexture == 1) base = srgbToLinear(base);
    if (uUseNormalMap == 1) {
        vec3 tangentNormal = texture(uNormalTexture, vUV).xyz * 2.0 - 1.0;
        vec3 dp1 = dFdx(vPosition), dp2 = dFdy(vPosition);
        vec2 duv1 = dFdx(vUV), duv2 = dFdy(vUV);
        vec3 tangent = normalize(dp1 * duv2.y - dp2 * duv1.y);
        vec3 bitangent = normalize(cross(n, tangent));
        n = normalize(mat3(tangent, bitangent, n) * tangentNormal);
    }
    float metallic = clamp(uMetallic, 0.0, 1.0);
    float roughness = clamp(uUseMetallicRoughness == 1 ? texture(uMetallicRoughnessTexture, vUV).g * uRoughness : uRoughness, 0.04, 1.0);
    if (uUseMetallicRoughness == 1) metallic = clamp(texture(uMetallicRoughnessTexture, vUV).b * uMetallic, 0.0, 1.0);
    vec3 viewDir = normalize(-vPosition);
    vec3 light = vec3(0.0, 0.0, 1.0); //头灯，视线z
    vec3 halfDir = normalize(viewDir + light);
    float nDotL = max(dot(n, light), 0.0);
    float nDotV = max(dot(n, viewDir), 0.0);
    vec3 f0 = mix(vec3(0.04), base, metallic);
    vec3 f = fresnelSchlick(max(dot(halfDir, viewDir), 0.0), f0);
    float d = distributionGGX(n, halfDir, roughness);
    float g = geometrySmith(n, viewDir, light, roughness);
    vec3 numerator = d * g * f;
    vec3 specular = numerator / max(4.0 * nDotV * nDotL, 0.0001);
    vec3 diffuse = (1.0 - f) * (1.0 - metallic) * base / 3.14159265;
    vec3 color = (diffuse + specular) * nDotL * 3.0 + base * 0.03;
    FragColor = vec4(linearToSrgb(color), 1.0);
}

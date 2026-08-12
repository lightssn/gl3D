//================ 顶点着色器 VERTEX ================
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

//================ 片段着色器 FRAGMENT ================
#version 460 core
in vec3 vColor;
out vec4 FragColor;
void main() { FragColor = vec4(vColor, 1.0); }

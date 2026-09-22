#version 450

layout(set = 0, binding = 0) uniform DrawParams {
    mat4 mvp;
    mat4 modelView;
    vec4 baseMetal;
    vec4 materialFlags;
    vec4 options;
} params;
layout(push_constant) uniform DrawMode { int mode; float outline; } drawMode;
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 0) out vec3 normal;
layout(location = 1) out vec2 uv;
layout(location = 2) out vec3 viewPosition;

void main() {
    vec3 position = inPosition + (drawMode.mode == 1 ? inNormal * drawMode.outline : vec3(0.0));
    gl_Position = params.mvp * vec4(position, 1.0);
    normal = mat3(transpose(inverse(params.modelView))) * inNormal;
    uv = inUV;
    viewPosition = (params.modelView * vec4(position, 1.0)).xyz;
}

#version 450

layout(set = 0, binding = 0) uniform DrawParams {
    mat4 mvp;
    mat4 modelView;
    vec4 baseMetal;
    vec4 materialFlags;
    vec4 options;
    vec4 tileTint;
} params;
layout(push_constant) uniform DrawMode { int mode; float outline; vec2 viewport; } drawMode;
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 0) out vec3 normal;
layout(location = 1) out vec2 uv;
layout(location = 2) out vec3 viewPosition;

void main() {
    vec3 position = inPosition;
    gl_Position = params.mvp * vec4(position, 1.0);
    normal = mat3(transpose(inverse(params.modelView))) * inNormal;
    if (drawMode.mode == 1 && drawMode.outline > 0.0 && gl_Position.w > 0.000001) {
        // 与OpenGL相同的像素外扩，包含Vulkan投影的Y翻转。
        mat4 projection = params.mvp * inverse(params.modelView);
        vec4 direction = projection * vec4(normal, 0.0);
        vec2 pixelDirection = (direction.xy - gl_Position.xy / gl_Position.w * direction.w) * drawMode.viewport;
        float lengthPixels = length(pixelDirection);
        if (lengthPixels > 0.000001)
            gl_Position.xy += pixelDirection / lengthPixels * (2.0 * drawMode.outline / drawMode.viewport) * gl_Position.w;
    }
    uv = inUV;
    viewPosition = (params.modelView * vec4(position, 1.0)).xyz;
}

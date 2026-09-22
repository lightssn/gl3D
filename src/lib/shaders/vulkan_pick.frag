#version 450

layout(set = 0, binding = 0) uniform DrawParams {
    mat4 mvp;
    mat4 modelView;
    vec4 baseMetal;
    vec4 materialFlags;
    vec4 options;
} params;
layout(location = 0) out uint objectId;

void main() {
    objectId = uint(params.options.w);
}

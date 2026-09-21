#version 450
layout(location = 0) in vec3 normal;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 n = normalize(normal);
    vec3 light = normalize(vec3(0.35, 0.8, 0.45));
    float diffuse = max(dot(n, light), 0.0);
    outColor = vec4(vec3(0.72, 0.76, 0.84) * (0.18 + diffuse * 0.82), 1.0);
}

#version 450

layout(set = 0, binding = 0) uniform DrawParams {
    mat4 mvp;
    mat4 modelView;
    vec4 baseMetal;
    vec4 materialFlags;
    vec4 options;
} params;
layout(set = 0, binding = 1) uniform sampler2D baseTexture;
layout(set = 0, binding = 2) uniform sampler2D metalRoughTexture;
layout(set = 0, binding = 3) uniform sampler2D normalTexture;
layout(push_constant) uniform DrawMode { int mode; float outline; } drawMode;
layout(location = 0) in vec3 normal;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 viewPosition;
layout(location = 0) out vec4 outColor;

vec3 srgbToLinear(vec3 value) {
    return mix(value / 12.92, pow((value + 0.055) / 1.055, vec3(2.4)),
               step(vec3(0.04045), value));
}
vec3 linearToSrgb(vec3 value) {
    value = max(value, vec3(0.0));
    return mix(value * 12.92, 1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055,
               step(vec3(0.0031308), value));
}
float distributionGGX(vec3 n, vec3 h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndh = max(dot(n, h), 0.0);
    float denominator = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * denominator * denominator, 0.0001);
}
float geometrySchlickGGX(float ndv, float roughness) {
    float r = roughness + 1.0;
    float k = r * r / 8.0;
    return ndv / max(ndv * (1.0 - k) + k, 0.0001);
}
vec3 fresnelSchlick(float cosine, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - clamp(cosine, 0.0, 1.0), 5.0);
}

void main() {
    if (drawMode.mode == 1) { outColor = vec4(0.9, 0.1, 0.1, 1.0); return; }
    if (drawMode.mode == 2) { outColor = vec4(1.0, 0.76, 0.82, 0.35); return; }
    vec3 n = normalize(normal);
    if (params.options.z > 0.5 && params.options.z < 1.5) {
        outColor = vec4(n * 0.5 + 0.5, 1.0); return;
    }
    if (params.options.z > 1.5) { outColor = vec4(fract(uv), 0.0, 1.0); return; }
    vec3 base = params.materialFlags.y > 0.5 ? texture(baseTexture, uv).rgb : params.baseMetal.rgb;
    if (params.options.y > 0.5 && params.materialFlags.w > 0.5) {
        vec3 tangentNormal = texture(normalTexture, uv).xyz * 2.0 - 1.0;
        vec3 dp1 = dFdx(viewPosition), dp2 = dFdy(viewPosition);
        vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
        vec3 tangent = normalize(dp1 * duv2.y - dp2 * duv1.y);
        vec3 bitangent = normalize(cross(n, tangent));
        n = normalize(mat3(tangent, bitangent, n) * tangentNormal);
    }
    vec3 light = vec3(0.0, 0.0, 1.0);
    if (params.options.x < 0.5) {
        float diffuse = abs(dot(n, light)) * 0.75;
        float specular = pow(max(abs(dot(n, light)), 0.0), 32.0) * 0.25;
        outColor = vec4(base * (0.30 + diffuse) + vec3(specular), 1.0);
        return;
    }
    if (params.materialFlags.y > 0.5) base = srgbToLinear(base);
    float metallic = clamp(params.baseMetal.a, 0.0, 1.0);
    float roughness = clamp(params.materialFlags.x, 0.04, 1.0);
    if (params.materialFlags.z > 0.5) {
        vec4 material = texture(metalRoughTexture, uv);
        metallic = clamp(material.b * metallic, 0.0, 1.0);
        roughness = clamp(material.g * roughness, 0.04, 1.0);
    }
    vec3 viewDirection = normalize(-viewPosition);
    vec3 halfDirection = normalize(viewDirection + light);
    float ndl = max(dot(n, light), 0.0);
    float ndv = max(dot(n, viewDirection), 0.0);
    vec3 f0 = mix(vec3(0.04), base, metallic);
    vec3 fresnel = fresnelSchlick(max(dot(halfDirection, viewDirection), 0.0), f0);
    float geometry = geometrySchlickGGX(ndv, roughness) * geometrySchlickGGX(ndl, roughness);
    vec3 specular = distributionGGX(n, halfDirection, roughness) * geometry * fresnel /
                    max(4.0 * ndv * ndl, 0.0001);
    vec3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * base / 3.14159265;
    vec3 color = (diffuse + specular) * ndl * 3.0 + base * 0.03;
    outColor = vec4(linearToSrgb(color), 1.0);
}

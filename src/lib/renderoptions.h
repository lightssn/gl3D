#pragma once

// Backend-neutral rendering configuration shared by OpenGL and Vulkan.
struct RenderOptions {
    float clearColor[3] = {0.13f,0.13f,0.16f};
    bool antialiasing = true;
    bool mipmaps = false;
    bool showGrid = true;
    bool showGizmo = true;
    bool showSelection = true;
    bool showAxes = true;
    bool depthTest = true;
    bool faceCulling = false;
    bool pbr = false;
    bool normalMap = true;
    bool normalLighting = true;
    bool wireframe = false;
    bool tileLevelTint = false;
    bool osgbOnDemand = true;
    bool showTileBounds = false;
    bool fixedFps = false;
    int targetFps = 60;
    int debugView = 0;
    float outlineWidth = 2.0f; // Screen-space width in physical pixels, independent of model scale.
    };

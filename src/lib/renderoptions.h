#pragma once

// Backend-neutral rendering configuration shared by OpenGL and Vulkan.
struct RenderOptions {
    bool antialiasing = true;
    bool showGrid = true;
    bool showGizmo = true;
    bool showSelection = true;
    bool showAxes = true;
    bool depthTest = true;
    bool faceCulling = false;
    bool pbr = false;
    bool normalMap = true;
    bool wireframe = false;
<<<<<<< HEAD
    bool tileLevelTint = false;
=======
    bool showTileBounds = false;
>>>>>>> d8eb4ffe194d0afa7bf43d9f71952bfa2058c50e
    bool fixedFps = false;
    int targetFps = 60;
    int debugView = 0;
    float outlineWidth = 1.0f;
};

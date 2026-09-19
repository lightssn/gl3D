# gl3d_core 动态库 模型加载+OpenGL渲染
add_library(gl3d_core SHARED
    glcontext.h
    debugstats.h
    renderoptions.h
    cameracontroller.h
    selectioncontroller.h
    openglshader.h
    openglmeshrenderer.h
    mesh.h
    mesh.cpp
    meshloader.h
    meshloader.cpp
    meshloader_glb.cpp
    camera.h
    camera.cpp
    shader.h
    shader.cpp
    meshrenderer.h
    meshrenderer.cpp
    glwidget.h
    glwidget.cpp
)

if(GL3D_HAS_VULKAN)
    target_sources(gl3d_core PRIVATE
        vulkanbackend.h
        vulkanbackend.cpp
    )
    target_compile_definitions(gl3d_core PUBLIC GL3D_HAS_VULKAN=1)
    target_link_libraries(gl3d_core PUBLIC Vulkan::Vulkan)
endif()

# GL3D_CORE_EXPORT 宏由编译器定义 dllexport/dllimport
target_compile_definitions(gl3d_core PRIVATE GL3D_CORE_BUILD)

target_include_directories(gl3d_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(gl3d_core PUBLIC
    ${GL3D_QT_WIDGETS_TARGET}
    ${GL3D_QT_OPENGL_TARGET}
    ${GL3D_QT_OPENGL_WIDGETS_TARGET}
)

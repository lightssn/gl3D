# gl3d_core 动态库 模型加载+OpenGL渲染
add_library(gl3d_core SHARED
    editcontroller.h
    editcontroller.cpp
    tilenode.h
    tilestream.h
    tilestream.cpp
    glcontext.h
    debugstats.h
    processmemory.h
    processmemory.cpp
    renderoptions.h
    cameracontroller.h
    selectioncontroller.h
    renderscene.h
    renderscene.cpp
    renderview.h
    renderframe.h
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
    glwindow.h
    glwindow.cpp
)

if(GL3D_ENABLE_OSGB)
    target_sources(gl3d_core PRIVATE osgbloader.h osgbloader.cpp)
    target_compile_definitions(gl3d_core PUBLIC GL3D_HAS_OSGB=1)
    target_link_libraries(gl3d_core PRIVATE unofficial::osg::osgDB unofficial::osg::osg)
endif()

if(GL3D_HAS_VULKAN)
    target_sources(gl3d_core PRIVATE
        vkwindow.h
        vkwindow.cpp
        vkwindow_input.cpp
    )
    target_compile_definitions(gl3d_core PUBLIC GL3D_HAS_VULKAN=1)
    target_link_libraries(gl3d_core PUBLIC Vulkan::Vulkan)
endif()

# GL3D_CORE_EXPORT 宏由编译器定义 dllexport/dllimport
target_compile_definitions(gl3d_core PRIVATE GL3D_CORE_BUILD)

target_include_directories(gl3d_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(gl3d_core PUBLIC
    ${GL3D_QT_WIDGETS_TARGET}
    ${GL3D_QT_GUI_TARGET}
    ${GL3D_QT_OPENGL_TARGET}
    ${GL3D_QT_OPENGL_WIDGETS_TARGET}
)

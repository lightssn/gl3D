# Deploy Qt for standalone test builds.
function(gl3d_deploy_test_qt target)
    if(NOT WIN32)
        return()
    endif()
    foreach(module Core Gui Widgets OpenGL OpenGLWidgets)
        set(qt_target "Qt${GL3D_QT_MAJOR_VERSION}::${module}")
        if(TARGET ${qt_target})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${qt_target}>" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
    endforeach()
    set(platform_target "Qt${GL3D_QT_MAJOR_VERSION}::QWindowsIntegrationPlugin")
    if(TARGET ${platform_target})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${platform_target}>" "$<TARGET_FILE_DIR:${target}>/platforms"
            VERBATIM)
    endif()
endfunction()

find_package(GTest CONFIG REQUIRED)

add_executable(gl3d_test
    test_meshloader.cpp
    test_camera.cpp
    test_main.cpp
)

target_link_libraries(gl3d_test PRIVATE
    gl3d_core
    GTest::gtest
)
if(GL3D_ENABLE_OSGB)
    target_sources(gl3d_test PRIVATE test_osgb.cpp)
    add_dependencies(gl3d_test gl3d_osgb_index)
    target_link_libraries(gl3d_test PRIVATE unofficial::osg::osgDB unofficial::osg::osg)
    gl3d_deploy_osg(gl3d_test)
endif()

gl3d_deploy_test_qt(gl3d_test)

include(GoogleTest)
gtest_discover_tests(gl3d_test)

add_executable(gl3d_gl_stream_test test_gl_stream.cpp)
target_link_libraries(gl3d_gl_stream_test PRIVATE gl3d_core)
gl3d_deploy_test_qt(gl3d_gl_stream_test)
add_test(NAME OpenGL.TileStreaming COMMAND gl3d_gl_stream_test)

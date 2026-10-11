if(GL3D_ENABLE_OSGB)
    find_path(GL3D_OSG_INCLUDE_DIR osg/NodeVisitor REQUIRED)
    add_executable(gl3d_osgb_index osgb_index.cpp)
    target_include_directories(gl3d_osgb_index PRIVATE ${GL3D_OSG_INCLUDE_DIR})
    target_link_libraries(gl3d_osgb_index PRIVATE Qt${GL3D_QT_MAJOR_VERSION}::Core unofficial::osg::osgDB unofficial::osg::osg)
    gl3d_deploy_osg(gl3d_osgb_index)
    if(WIN32)
        add_custom_command(TARGET gl3d_osgb_index POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt${GL3D_QT_MAJOR_VERSION}::Core>" "$<TARGET_FILE_DIR:gl3d_osgb_index>"
            VERBATIM)
    endif()
endif()

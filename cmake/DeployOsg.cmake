# 只部署读取摄影测量 OSGB 所需的插件与图片编解码运行库。
function(gl3d_deploy_osg target)
    if(NOT GL3D_ENABLE_OSGB OR NOT WIN32)
        return()
    endif()
    get_filename_component(osg_runtime_root "${unofficial-osg_DIR}/../.." ABSOLUTE)
    foreach(plugin osg serializers_osg jpeg png dds rgb bmp tga)
        if(TARGET unofficial::osg::osgdb_${plugin})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/osgPlugins-3.6.5"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:unofficial::osg::osgdb_${plugin}>"
                    "$<TARGET_FILE_DIR:${target}>/osgPlugins-3.6.5"
                VERBATIM)
        endif()
    endforeach()
    foreach(configuration release debug)
        if(configuration STREQUAL "debug")
            set(runtime_dir "${osg_runtime_root}/debug/bin")
        else()
            set(runtime_dir "${osg_runtime_root}/bin")
        endif()
        file(GLOB image_runtimes "${runtime_dir}/jpeg*.dll" "${runtime_dir}/libpng*.dll"
            "${runtime_dir}/zlib*.dll" "${runtime_dir}/z.dll" "${runtime_dir}/libexpat*.dll")
        foreach(runtime IN LISTS image_runtimes)
            if(configuration STREQUAL "debug")
                set(copy_source "$<$<CONFIG:Debug>:${runtime}>")
            else()
                set(copy_source "$<$<NOT:$<CONFIG:Debug>>:${runtime}>")
            endif()
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} "-DSOURCE=${copy_source}"
                    "-DDESTINATION=$<TARGET_FILE_DIR:${target}>"
                    -P "${PROJECT_SOURCE_DIR}/cmake/CopyRuntime.cmake"
                VERBATIM)
        endforeach()
    endforeach()
endfunction()

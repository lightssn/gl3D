# gl3d 可执行程序 CLI+GUI入口
add_executable(gl3d
    main.cpp
    mainwindow.h
    mainwindow.cpp
)

target_link_libraries(gl3d PRIVATE
    gl3d_core
    Qt6::Widgets
)

# 主题文件部署到exe同目录
foreach(qss theme_night.qss theme_day.qss)
    add_custom_command(TARGET gl3d POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${qss}
            $<TARGET_FILE_DIR:gl3d>/${qss})
endforeach()

# windeployqt自动拷贝Qt运行时dll与插件
find_program(WINDEPLOYQT windeployqt HINTS "${CMAKE_PREFIX_PATH}/bin")
if(WINDEPLOYQT)
    add_custom_command(TARGET gl3d POST_BUILD
        COMMAND ${WINDEPLOYQT} --no-translations --no-system-d3d-compiler
            $<TARGET_FILE:gl3d>)
endif()

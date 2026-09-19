# gl3d 可执行程序 CLI+GUI入口
add_executable(gl3d
    main.cpp
    mainwindow.h
    mainwindow.cpp
    debugwindow.h
    debugwindow.cpp
)

target_link_libraries(gl3d PRIVATE
    gl3d_core
    ${GL3D_QT_WIDGETS_TARGET}
)

# 主题文件部署到exe同目录
foreach(qss theme_night.qss theme_day.qss)
    add_custom_command(TARGET gl3d POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${qss}
            $<TARGET_FILE_DIR:gl3d>/${qss})
endforeach()

# 着色器文件部署到exe同目录shaders/ 仿UE.usf运行时加载
# 顶点+片段合并一个.glsl文件 按#version切分
file(GLOB shader_files ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl)
add_custom_command(TARGET gl3d POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:gl3d>/shaders)
foreach(shader ${shader_files})
    get_filename_component(sname ${shader} NAME)
    add_custom_command(TARGET gl3d POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${shader}
            $<TARGET_FILE_DIR:gl3d>/shaders/${sname})
endforeach()

# windeployqt自动拷贝Qt运行时dll与插件。windeployqt需要调用同版本qmake。
find_program(WINDEPLOYQT windeployqt HINTS "${CMAKE_PREFIX_PATH}/bin")
if(WINDEPLOYQT)
    get_filename_component(WINDEPLOYQT_BIN_DIR "${WINDEPLOYQT}" DIRECTORY)
    # Qt6通常与windeployqt同目录；vcpkg Qt5可能需要从tools/qt5/bin回退查找。
    if(EXISTS "${WINDEPLOYQT_BIN_DIR}/qmake.exe")
        set(QMAKE_EXECUTABLE "${WINDEPLOYQT_BIN_DIR}/qmake.exe")
    else()
        find_program(QMAKE_EXECUTABLE qmake
            HINTS
                "${WINDEPLOYQT_BIN_DIR}/../../qt5/bin"
                "${CMAKE_PREFIX_PATH}/bin"
            NO_DEFAULT_PATH
        )
    endif()
endif()

if(WINDEPLOYQT AND QMAKE_EXECUTABLE)
    get_filename_component(QMAKE_BIN_DIR "${QMAKE_EXECUTABLE}" DIRECTORY)
    add_custom_command(TARGET gl3d POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=${QMAKE_BIN_DIR};$ENV{PATH}"
            ${WINDEPLOYQT} --no-translations --no-angle --no-system-d3d-compiler
            $<TARGET_FILE:gl3d>)
elseif(WINDEPLOYQT)
    message(WARNING "windeployqt was found, but qmake was not found; Qt runtime deployment is skipped")
endif()

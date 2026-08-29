cmake_minimum_required(VERSION 3.21)
project(gl3D LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    add_compile_options(/utf-8) # 中文注释与中文路径
endif()

# 统一输出目录 exe与dll同目录 便于windeployqt部署
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

find_package(Qt6 QUIET COMPONENTS Widgets OpenGL OpenGLWidgets)
if(Qt6_FOUND)
    set(GL3D_QT_MAJOR_VERSION 6)
    set(GL3D_QT_WIDGETS_TARGET Qt6::Widgets)
    set(GL3D_QT_OPENGL_TARGET Qt6::OpenGL)
    set(GL3D_QT_OPENGL_WIDGETS_TARGET Qt6::OpenGLWidgets)
else()
    find_package(Qt5 REQUIRED COMPONENTS Widgets OpenGL)
    set(GL3D_QT_MAJOR_VERSION 5)
    set(GL3D_QT_WIDGETS_TARGET Qt5::Widgets)
    set(GL3D_QT_OPENGL_TARGET Qt5::OpenGL)
    set(GL3D_QT_OPENGL_WIDGETS_TARGET)
endif()

message(STATUS "Using Qt ${GL3D_QT_MAJOR_VERSION}")

add_subdirectory(src/lib)
add_subdirectory(src/app)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(test)
endif()

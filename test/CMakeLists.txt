find_package(GTest CONFIG REQUIRED)

add_executable(gl3d_test
    test_meshloader.cpp
    test_osgbstream.cpp
)

target_compile_definitions(gl3d_test PRIVATE GL3D_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(gl3d_test PRIVATE
    gl3d_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(gl3d_test)

find_package(GTest CONFIG REQUIRED)

add_executable(gl3d_test
    test_meshloader.cpp
)

target_link_libraries(gl3d_test PRIVATE
    gl3d_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(gl3d_test)

cmake_minimum_required(VERSION 3.20)

add_executable(matrix_detection_test matrix_detection_test.cpp)
target_compile_features(matrix_detection_test PRIVATE cxx_std_17)

# Register with CTest
include(CTest)
add_test(NAME MatrixDetection COMMAND matrix_detection_test)

# CMake generated Testfile for 
# Source directory: C:/Users/Anakin/OpenAnsel/src/tests
# Build directory: C:/Users/Anakin/OpenAnsel/build64/src/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[MatrixDetection]=] "C:/Users/Anakin/OpenAnsel/build64/src/tests/Debug/matrix_detection_test.exe")
  set_tests_properties([=[MatrixDetection]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Anakin/OpenAnsel/src/tests/CMakeLists.txt;8;add_test;C:/Users/Anakin/OpenAnsel/src/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[MatrixDetection]=] "C:/Users/Anakin/OpenAnsel/build64/src/tests/Release/matrix_detection_test.exe")
  set_tests_properties([=[MatrixDetection]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Anakin/OpenAnsel/src/tests/CMakeLists.txt;8;add_test;C:/Users/Anakin/OpenAnsel/src/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[MatrixDetection]=] "C:/Users/Anakin/OpenAnsel/build64/src/tests/MinSizeRel/matrix_detection_test.exe")
  set_tests_properties([=[MatrixDetection]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Anakin/OpenAnsel/src/tests/CMakeLists.txt;8;add_test;C:/Users/Anakin/OpenAnsel/src/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[MatrixDetection]=] "C:/Users/Anakin/OpenAnsel/build64/src/tests/RelWithDebInfo/matrix_detection_test.exe")
  set_tests_properties([=[MatrixDetection]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Anakin/OpenAnsel/src/tests/CMakeLists.txt;8;add_test;C:/Users/Anakin/OpenAnsel/src/tests/CMakeLists.txt;0;")
else()
  add_test([=[MatrixDetection]=] NOT_AVAILABLE)
endif()

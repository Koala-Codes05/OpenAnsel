cmake_minimum_required(VERSION 3.20)

add_executable(openansel_launcher WIN32
    launcher.cpp
)

target_compile_features(openansel_launcher PRIVATE cxx_std_17)

set_target_properties(openansel_launcher PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_link_libraries(openansel_launcher PRIVATE comctl32)

add_library(openansel_io STATIC
    image_writer.cpp
    image_writer.h
)

target_include_directories(openansel_io PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(openansel_io PUBLIC
    Microsoft::DirectXTex
    windowscodecs
)

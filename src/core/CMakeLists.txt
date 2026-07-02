add_library(openansel_core STATIC
    core.cpp
    core.h
)

target_include_directories(openansel_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(openansel_core PUBLIC
)

add_executable(openansel_injector injector.cpp)

target_include_directories(openansel_injector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

set_target_properties(openansel_injector PROPERTIES
    OUTPUT_NAME "openansel_injector"
)

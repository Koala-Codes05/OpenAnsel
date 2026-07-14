# src/backends/dxgi/CMakeLists.txt

add_library(openansel_dxgi SHARED
    dxgi_proxy.cpp
    dxgi.def
    dxgi_backend.cpp
    dxgi_backend.h
    dx11_hooks.cpp
    dx11_hooks.h
    dx12_hooks.cpp
    dx12_hooks.h
)

target_include_directories(openansel_dxgi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(openansel_dxgi PUBLIC
    openansel_core
    openansel_math
    openansel_io
    minhook::minhook
    dxguid
    d3d11
    d3d12
    user32
)

set_target_properties(openansel_dxgi PROPERTIES
    OUTPUT_NAME "dxgi"
)

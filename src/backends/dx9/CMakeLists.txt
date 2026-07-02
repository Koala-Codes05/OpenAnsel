# ─── Overlay static library (isolated imgui linkage) ─────────────────────────
# dx9_overlay.cpp is compiled as a SEPARATE static library so that imgui's
# INTERFACE_INCLUDE_DIRECTORIES and INTERFACE_COMPILE_DEFINITIONS are NOT
# propagated to the other TUs in openansel_dx9.  d3d9_proxy.cpp in particular
# breaks with C2039/C2873 cstdio errors when imgui's CRT definitions leak into
# its compilation environment.  PRIVATE linkage here prevents that propagation.
add_library(openansel_dx9_overlay STATIC
    dx9_overlay.cpp
    dx9_overlay.h
)

target_include_directories(openansel_dx9_overlay PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}  # dx9_overlay.h, dx9_hooks.h
    ${CMAKE_SOURCE_DIR}/src      # <core/core.h>
)

target_link_libraries(openansel_dx9_overlay PRIVATE
    imgui::imgui
    openansel_core
    d3d9
    user32
)

# ─── Main DX9 proxy / hook DLL ────────────────────────────────────────────────
add_library(openansel_dx9 SHARED
    dx9_backend.cpp
    dx9_backend.h
    dx9_hooks.cpp
    dx9_hooks.h
    d3d9_proxy.cpp
    d3d9.def
)

target_include_directories(openansel_dx9 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(openansel_dx9 PUBLIC
    openansel_core
    openansel_math
    openansel_io
    minhook::minhook
    openansel_dx9_overlay   # imgui is PRIVATE to this target - does not propagate
    d3d9
    dxguid
    gdi32
    user32
)

# Output as d3d9.dll so it can be dropped next to the game exe as a proxy DLL.
# The injector still works by loading the DLL by full path if preferred.
set_target_properties(openansel_dx9 PROPERTIES
    OUTPUT_NAME "d3d9"
)

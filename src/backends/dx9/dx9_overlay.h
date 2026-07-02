#pragma once

#define WIN32_LEAN_AND_MEAN
#include <d3d9.h>
#include <windows.h>
#include <cstdint>

namespace OpenAnsel::DX9 {

// Initialise the ImGui overlay for the given device and window.
// Safe to call multiple times; subsequent calls are no-ops.
void InitOverlay(IDirect3DDevice9* device, HWND hwnd);

// Tear down ImGui and restore the original WndProc.
void ShutdownOverlay();

// Call from EndScene_Hook (before EndScene_Original) to build and render the overlay.
// No-op if InitOverlay has not been called or the overlay is toggled off.
void RenderOverlay();

extern uint32_t g_cfgTileCountX;
extern uint32_t g_cfgTileCountY;

} // namespace OpenAnsel::DX9

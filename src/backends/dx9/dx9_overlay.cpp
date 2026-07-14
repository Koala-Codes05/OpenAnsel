// dx9_overlay.cpp
// ImGui in-game overlay for OpenAnsel.
//
// Renders a compact sidebar showing capture state, tile progress, and the last
// saved filename.  Press F9 to toggle the overlay on / off.
//
// Include order matters on MSVC x86:
//   1. <stdio.h> / <stdlib.h> must come first so the CRT functions are declared
//      in the global namespace before any Windows SDK headers can interfere.
//   2. <windows.h> next (with WIN32_LEAN_AND_MEAN / NOMINMAX guards).
//   3. imgui.h then its backends — before <d3d9.h>.
//      vcpkg installs the backends FLAT (no "backends/" subdirectory).
//   4. D3D / project headers last.
// Deviating from this order causes C2039 ('fsetpos not a member of ::') on
// MSVC x86 because certain Windows SDK headers redefine CRT symbols as macros.

// ─── Step 1: C runtime headers (must precede all Windows/DX/imgui headers) ────
#include <stdio.h>
#include <stdlib.h>

// ─── Step 2: Windows headers ─────────────────────────────────────────────────
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// ─── Step 3: imgui (before <d3d9.h> — canonical imgui DX9 example order) ────
// vcpkg installs backend headers flat: <imgui_impl_dx9.h>, not <backends/...>
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

// ─── Step 4: project headers (dx9_overlay.h brings in <d3d9.h>) ─────────────
#include "dx9_overlay.h"
#include "dx9_hooks.h"

#include <core/core.h>

#include <algorithm>
#include <cstring>
#include <string>

// Forward-declare the imgui Win32 WndProc handler.
// imgui_impl_win32.h puts this in a #if 0 block to avoid dragging in <windows.h>;
// we copy it here per the imgui documentation.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                              WPARAM wParam, LPARAM lParam);

namespace OpenAnsel::DX9 {

uint32_t g_cfgTileCountX = 2;
uint32_t g_cfgTileCountY = 2;

namespace {

static bool       g_initialized   = false;
static bool       g_showOverlay   = true;   // toggled by F9
static HWND       g_hwnd          = nullptr;
static WNDPROC    g_origWndProc   = nullptr;

static void LogWriter(const char* msg) {
    FILE* f = nullptr;
    if (fopen_s(&f, "OpenAnsel_dx9.log", "a") == 0 && f) { fprintf(f, "%s\n", msg); fclose(f); }
}

// ─── WndProc subclass ─────────────────────────────────────────────────────────

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_initialized && g_showOverlay) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            return TRUE;

        const ImGuiIO& io = ImGui::GetIO();
        if ((io.WantCaptureMouse && (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)) ||
            (io.WantCaptureKeyboard && (msg >= WM_KEYFIRST && msg <= WM_KEYLAST))) {
            return TRUE;
        }
    }
    if (g_origWndProc) {
        return CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Helper: wstring → UTF-8 for ImGui ───────────────────────────────────────

std::string WstrToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                                        nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                        out.data(), len, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

// ─── Sidebar UI ───────────────────────────────────────────────────────────────

void BuildOverlayUI() {
    const ImGuiIO& io = ImGui::GetIO();

    // Anchor to the top-right corner with a small margin.
    const float PAD = 15.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - PAD, PAD),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(280.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration       |
        ImGuiWindowFlags_NoNav              |
        ImGuiWindowFlags_NoMove             |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!ImGui::Begin("##OpenAnselOverlay", nullptr, flags)) {
        ImGui::End();
        return;
    }

    // ── Header ────────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.00f, 0.75f, 1.00f, 1.00f));
    ImGui::Text("  OpenAnsel Dashboard");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Settings ──────────────────────────────────────────────────────────────
    const CaptureState state = Core::Instance().GetState();
    const CaptureProgressInfo prog = QueryCaptureProgress();

    const bool isCapturing = (state == CaptureState::Capturing || state == CaptureState::Requested || state == CaptureState::Processing);

    ImGui::Text("Grid Resolution:");
    
    if (isCapturing) {
        ImGui::BeginDisabled();
    }
    
    int tx = static_cast<int>(g_cfgTileCountX);
    int ty = static_cast<int>(g_cfgTileCountY);
    
    ImGui::PushItemWidth(100.0f);
    if (ImGui::SliderInt("Columns", &tx, 1, 8)) {
        g_cfgTileCountX = static_cast<uint32_t>(tx);
    }
    if (ImGui::SliderInt("Rows", &ty, 1, 8)) {
        g_cfgTileCountY = static_cast<uint32_t>(ty);
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::Text("Presets:");
    if (ImGui::Button("2x2 (4x pixels)")) { g_cfgTileCountX = 2; g_cfgTileCountY = 2; }
    ImGui::SameLine();
    if (ImGui::Button("4x4 (16x)"))       { g_cfgTileCountX = 4; g_cfgTileCountY = 4; }
    ImGui::SameLine();
    if (ImGui::Button("8x8 (64x)"))       { g_cfgTileCountX = 8; g_cfgTileCountY = 8; }
    
    if (isCapturing) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Capture Action Button ────────────────────────────────────────────────
    if (isCapturing) {
        ImGui::BeginDisabled();
    }

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.45f, 0.70f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.55f, 0.85f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.35f, 0.55f, 1.00f));
    
    if (ImGui::Button("Start Capture", ImVec2(-1.0f, 32.0f))) {
        CaptureConfig config;
        config.tileCountX = g_cfgTileCountX;
        config.tileCountY = g_cfgTileCountY;
        Core::Instance().RequestCapture(config);
    }
    
    ImGui::PopStyleColor(3);

    if (isCapturing) {
        ImGui::EndDisabled();
    }

    // ── Free Camera Section ──────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.00f, 0.75f, 1.00f, 1.00f));
    ImGui::Text("  Free Camera (Experimental)");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    FreeCamConfig camCfg = Core::Instance().GetFreeCamConfig();
    bool camEnabled = camCfg.enabled;
    if (ImGui::Checkbox("Enable Free Camera [F8]", &camEnabled)) {
        camCfg.enabled = camEnabled;
        Core::Instance().SetFreeCamConfig(camCfg);
    }

    if (camCfg.enabled) {
        ImGui::SliderFloat("Move Speed", &camCfg.moveSpeed, 0.1f, 10.0f, "%.1fx");
        ImGui::SliderFloat("Look Speed", &camCfg.lookSpeed, 0.1f, 5.0f, "%.1fx");
        Core::Instance().SetFreeCamConfig(camCfg);
    }

    FreeCamStatusInfo camStatus = QueryFreeCamStatus();
    ImGui::Spacing();
    ImGui::Text("FreeCam Status: ");
    ImGui::SameLine();
    if (camStatus.active) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.20f, 1.00f, 0.40f, 1.0f));
        ImGui::TextUnformatted("Active");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
        ImGui::TextUnformatted("Disabled");
        ImGui::PopStyleColor();
    }

    if (camStatus.vpRegisterTracked) {
        ImGui::Text("VP Register: Tracked (reg %d)", camStatus.vpRegister);
    } else {
        ImGui::Text("VP Register: Not Tracked");
    }

    ImGui::Text("Substitution: %s", camStatus.substitutionHappened ? "Yes" : "No");
    if (camStatus.active) {
        ImGui::Text("Camera Pos: (%.2f, %.2f, %.2f)", camStatus.cameraPos[0], camStatus.cameraPos[1], camStatus.cameraPos[2]);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Status ────────────────────────────────────────────────────────────────
    const char* stateLabel = "Idle";
    ImVec4 stateColor = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);

    switch (state) {
    case CaptureState::Idle:
        stateLabel = "Idle";
        stateColor = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        break;
    case CaptureState::Requested:
        stateLabel = "Requested";
        stateColor = ImVec4(1.00f, 0.85f, 0.20f, 1.0f);
        break;
    case CaptureState::Capturing:
        stateLabel = "Capturing";
        stateColor = ImVec4(0.20f, 1.00f, 0.40f, 1.0f);
        break;
    case CaptureState::Processing:
        stateLabel = "Processing";
        stateColor = ImVec4(0.20f, 0.70f, 1.00f, 1.0f);
        break;
    case CaptureState::Error:
        stateLabel = "Error";
        stateColor = ImVec4(1.00f, 0.30f, 0.30f, 1.0f);
        break;
    }

    ImGui::Text("State: ");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
    ImGui::TextUnformatted(stateLabel);
    ImGui::PopStyleColor();

    // ── Tile progress bar ─────────────────────────────────────────────────────
    if (state == CaptureState::Capturing && prog.totalTiles > 0) {
        const float fraction = static_cast<float>(prog.currentTile) /
                               static_cast<float>(prog.totalTiles);
        char overlay_text[32];
        snprintf(overlay_text, sizeof(overlay_text),
                 "%u / %u tiles", prog.currentTile, prog.totalTiles);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay_text);
    }

    // ── Last saved path ───────────────────────────────────────────────────────
    if (!prog.lastSavedPath.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.70f, 1.0f));
        ImGui::TextUnformatted("Last saved:");
        const std::wstring& fullPath = prog.lastSavedPath;
        const size_t slash = fullPath.find_last_of(L"\\/");
        const std::wstring basename = (slash != std::wstring::npos)
                                          ? fullPath.substr(slash + 1)
                                          : fullPath;
        const std::string utf8base = WstrToUtf8(basename);
        ImGui::TextUnformatted(utf8base.c_str());
        ImGui::PopStyleColor();
        
        const std::string utf8FullPath = WstrToUtf8(fullPath);
        if (ImGui::Button("Copy Full Path")) {
            ImGui::SetClipboardText(utf8FullPath.c_str());
        }
    }

    // ── Hotkey reminder ───────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
    ImGui::TextUnformatted("F11 Capture | F9 Show/Hide Menu");
    ImGui::PopStyleColor();

    ImGui::End();
}

} // anonymous namespace

// ─── Public API ───────────────────────────────────────────────────────────────

void InitOverlay(IDirect3DDevice9* device, HWND hwnd) {
    if (g_initialized) return;
    if (!device || !hwnd) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't write imgui.ini next to the game exe

    // ── Style ─────────────────────────────────────────────────────────────────
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.WindowBorderSize  = 1.0f;
    style.Colors[ImGuiCol_Border]        = ImVec4(0.25f, 0.55f, 0.75f, 0.50f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.30f, 0.45f, 1.00f);
    style.Colors[ImGuiCol_WindowBg]      = ImVec4(0.08f, 0.08f, 0.10f, 0.90f);

    // ── Backends ──────────────────────────────────────────────────────────────
    if (!ImGui_ImplWin32_Init(hwnd)) { ImGui::DestroyContext(); return; }
    if (!ImGui_ImplDX9_Init(device)) { ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); return; }

    // ── Subclass the game window's WndProc ────────────────────────────────────
    g_hwnd = hwnd;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(OverlayWndProc)));
    if (!g_origWndProc) {
        ImGui_ImplDX9_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
        g_hwnd = nullptr;
        return;
    }

    g_initialized = true;
    LogWriter("InitOverlay: initialized");
}

void ShutdownOverlay() {
    if (!g_initialized) return;

    if (g_hwnd && g_origWndProc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_origWndProc = nullptr;
        g_hwnd = nullptr;
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_initialized = false;
}

void RenderOverlay() {
    if (!g_initialized) return;

    // F9 toggles visibility (reliably via state change tracking)
    static bool s_wasDown = false;
    bool isDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (isDown && !s_wasDown) {
        g_showOverlay = !g_showOverlay;
    }
    s_wasDown = isDown;

    if (!g_showOverlay) {
        ImGui::GetIO().MouseDrawCursor = false;
        return;
    }

    ImGui::GetIO().MouseDrawCursor = true;

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    BuildOverlayUI();

    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

} // namespace OpenAnsel::DX9

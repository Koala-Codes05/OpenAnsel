#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <thread>
#include <chrono>
#pragma comment(lib, "comctl32.lib")

// ─── Control IDs ──────────────────────────────────────────────────────────────
#define IDC_CB_RES      101
#define IDC_RB_FULL     102
#define IDC_RB_BORDER   103
#define IDC_RB_WINDOW   104
#define IDC_BTN_LAUNCH  105
#define IDC_BTN_EXIT    106
#define IDC_LBL_STATUS  107
#define IDC_GB_RES      108
#define IDC_GB_MODE     109

// ─── Resolutions ──────────────────────────────────────────────────────────────
struct Res { int w, h; const wchar_t* label; };
static const Res kRes[] = {
    { 1280,  720, L"1280 x 720  (HD)"      },
    { 1280,  800, L"1280 x 800"            },
    { 1600,  900, L"1600 x 900"            },
    { 1920, 1080, L"1920 x 1080 (Full HD)" },
    { 2560, 1440, L"2560 x 1440 (QHD)"    },
    { 3840, 2160, L"3840 x 2160 (4K)"     },
};
static const int kResCount = (int)(sizeof(kRes) / sizeof(kRes[0]));
enum Mode { FULLSCREEN = 0, BORDERLESS = 1, WINDOWED = 2 };

// ─── State ────────────────────────────────────────────────────────────────────
static int  g_resIdx = 3;
static Mode g_mode   = FULLSCREEN;
static wchar_t g_dir[MAX_PATH];
static wchar_t g_ini[MAX_PATH];
static HWND g_hwnd   = nullptr;
static HWND g_status = nullptr;

static void SetStatus(const wchar_t* msg) {
    if (g_status) SetWindowTextW(g_status, msg);
}

// ─── Config ───────────────────────────────────────────────────────────────────
static void LoadConfig() {
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    wchar_t* p = wcsrchr(g_dir, L'\\');
    if (p) *p = L'\0';
    swprintf_s(g_ini, L"%s\\OpenAnsel_launcher.ini", g_dir);
    g_resIdx = (int)GetPrivateProfileIntW(L"Launcher", L"ResIndex", 3, g_ini);
    g_mode   = (Mode)GetPrivateProfileIntW(L"Launcher", L"Mode", 0, g_ini);
    if (g_resIdx < 0 || g_resIdx >= kResCount) g_resIdx = 3;
    if ((int)g_mode < 0 || (int)g_mode > 2)    g_mode   = FULLSCREEN;
}

static void SaveConfig() {
    wchar_t b[8];
    swprintf_s(b, L"%d", g_resIdx); WritePrivateProfileStringW(L"Launcher", L"ResIndex", b, g_ini);
    swprintf_s(b, L"%d", g_mode);   WritePrivateProfileStringW(L"Launcher", L"Mode",     b, g_ini);
}

static void WriteMirrorsEdgeConfig(int w, int h, Mode mode) {
    wchar_t userProfile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH) == 0) return;

    wchar_t cfgPath[MAX_PATH];
    swprintf_s(cfgPath, L"%s\\Documents\\EA Games\\Mirror's Edge\\TdGame\\Config\\TdEngine.ini",
               userProfile);

    const wchar_t* sec = L"SystemSettings";
    wchar_t bw[16], bh[16];
    swprintf_s(bw, L"%d", w);
    swprintf_s(bh, L"%d", h);
    const wchar_t* fsVal = (mode == FULLSCREEN) ? L"True" : L"False";
    WritePrivateProfileStringW(sec, L"ResX",       bw,    cfgPath);
    WritePrivateProfileStringW(sec, L"ResY",       bh,    cfgPath);
    WritePrivateProfileStringW(sec, L"Fullscreen", fsVal, cfgPath);
}

// ─── Injection ────────────────────────────────────────────────────────────────
static DWORD FindPid(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) do {
        if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return pid;
}

static bool InjectDll(DWORD pid, const wchar_t* dllPath) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return false;
    size_t bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { CloseHandle(proc); return false; }
    WriteProcessMemory(proc, mem, dllPath, bytes, nullptr);
    auto llw = (LPTHREAD_START_ROUTINE)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(proc, nullptr, 0, llw, mem, 0, nullptr);
    bool ok = t != nullptr;
    if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

// ─── Borderless window ────────────────────────────────────────────────────────
struct FindWndCtx { DWORD pid; HWND hwnd; };
static BOOL CALLBACK FindMainWnd(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindWndCtx*>(lp);
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != ctx->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r = {}; GetWindowRect(h, &r);
    if ((r.right - r.left) < 200) return TRUE;
    ctx->hwnd = h;
    return FALSE;
}

static void ApplyBorderless(DWORD pid) {
    HWND hwnd = nullptr;
    for (int i = 0; i < 30 && !hwnd; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        FindWndCtx ctx{ pid, nullptr };
        EnumWindows(FindMainWnd, (LPARAM)&ctx);
        hwnd = ctx.hwnd;
    }
    if (!hwnd) return;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int sw = mi.rcMonitor.right  - mi.rcMonitor.left;
    int sh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    LONG s  = GetWindowLong(hwnd, GWL_STYLE);
    s &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    SetWindowLong(hwnd, GWL_STYLE, s);
    LONG es = GetWindowLong(hwnd, GWL_EXSTYLE);
    es &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLong(hwnd, GWL_EXSTYLE, es);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, sw, sh,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

// ─── Launch thread ────────────────────────────────────────────────────────────
static void LaunchThread() {
    const Res& r = kRes[g_resIdx];
    wchar_t exe[MAX_PATH], args[512], dll[MAX_PATH];
    swprintf_s(exe, L"%s\\MirrorsEdge.exe", g_dir);
    swprintf_s(dll, L"%s\\openansel_dx9.dll", g_dir);

    // Write resolution + display mode to Mirror's Edge config (more reliable than CLI args)
    WriteMirrorsEdgeConfig(r.w, r.h, g_mode);

    // Also pass CLI args as fallback
    if (g_mode == FULLSCREEN)
        swprintf_s(args, L"\"%s\" -ResX=%d -ResY=%d", exe, r.w, r.h);
    else
        swprintf_s(args, L"\"%s\" -ResX=%d -ResY=%d -windowed", exe, r.w, r.h);

    SetStatus(L"Starting Mirror's Edge...");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, args, nullptr, nullptr, FALSE, 0, nullptr, g_dir, &si, &pi)) {
        SetStatus(L"Failed to start game. Check the game path.");
        EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_LAUNCH), TRUE);
        return;
    }
    CloseHandle(pi.hThread);
    DWORD gamePid = pi.dwProcessId;

    if (g_mode == BORDERLESS)
        std::thread([gamePid]{ ApplyBorderless(gamePid); }).detach();

    SetStatus(L"Waiting 8s for D3D9 to initialize...");
    for (int i = 8; i > 0; --i) {
        wchar_t buf[64]; swprintf_s(buf, L"Waiting %ds for D3D9 to initialize...", i);
        SetStatus(buf);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exitCode) || exitCode != STILL_ACTIVE) {
        SetStatus(L"Game exited before injection. Check game files.");
        CloseHandle(pi.hProcess);
        EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_LAUNCH), TRUE);
        return;
    }

    SetStatus(L"Injecting OpenAnsel hook...");
    if (InjectDll(gamePid, dll))
        SetStatus(L"Injected!  Press F10 in-game to capture a screenshot.");
    else
        SetStatus(L"Injection failed — try running launcher as Administrator.");

    CloseHandle(pi.hProcess);
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_LAUNCH), TRUE);
}

// ─── Window procedure ─────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto mkLabel = [&](const wchar_t* t, int x, int y, int w, int h, int id, DWORD exStyle = 0) {
            HWND c = CreateWindowExW(exStyle, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     x, y, w, h, hwnd, (HMENU)(UINT_PTR)id, nullptr, nullptr);
            SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
            return c;
        };
        auto mkCtrl = [&](const wchar_t* cls, const wchar_t* t, DWORD style,
                          int x, int y, int w, int h, int id) {
            HWND c = CreateWindowW(cls, t, WS_CHILD | WS_VISIBLE | style,
                                   x, y, w, h, hwnd, (HMENU)(UINT_PTR)id, nullptr, nullptr);
            SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
            return c;
        };

        mkCtrl(L"BUTTON", L"Resolution", BS_GROUPBOX, 10, 10, 340, 58, IDC_GB_RES);
        HWND cb = mkCtrl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL,
                         20, 30, 320, 200, IDC_CB_RES);
        for (int i = 0; i < kResCount; ++i)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)kRes[i].label);
        SendMessage(cb, CB_SETCURSEL, g_resIdx, 0);

        mkCtrl(L"BUTTON", L"Display Mode", BS_GROUPBOX, 10, 78, 340, 90, IDC_GB_MODE);
        const wchar_t* rbLabels[] = { L"Fullscreen", L"Borderless Window", L"Windowed" };
        const int      rbIds[]    = { IDC_RB_FULL, IDC_RB_BORDER, IDC_RB_WINDOW };
        for (int i = 0; i < 3; ++i) {
            DWORD st = BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0);
            mkCtrl(L"BUTTON", rbLabels[i], st, 20, 96 + i * 22, 310, 20, rbIds[i]);
        }
        CheckRadioButton(hwnd, IDC_RB_FULL, IDC_RB_WINDOW, IDC_RB_FULL + (int)g_mode);

        g_status = mkLabel(L"Ready. Select options and click Launch.", 10, 176, 340, 18, IDC_LBL_STATUS);
        mkCtrl(L"BUTTON", L"Launch Game", BS_DEFPUSHBUTTON, 10, 200, 165, 30, IDC_BTN_LAUNCH);
        mkCtrl(L"BUTTON", L"Exit",        BS_PUSHBUTTON,    185, 200, 165, 30, IDC_BTN_EXIT);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CB_RES:
            if (HIWORD(wp) == CBN_SELCHANGE)
                g_resIdx = (int)SendDlgItemMessageW(hwnd, IDC_CB_RES, CB_GETCURSEL, 0, 0);
            break;
        case IDC_RB_FULL:   g_mode = FULLSCREEN; break;
        case IDC_RB_BORDER: g_mode = BORDERLESS; break;
        case IDC_RB_WINDOW: g_mode = WINDOWED;   break;
        case IDC_BTN_LAUNCH:
            SaveConfig();
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_LAUNCH), FALSE);
            std::thread(LaunchThread).detach();
            break;
        case IDC_BTN_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── WinMain ──────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    InitCommonControls();
    LoadConfig();

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OpenAnselLauncher";
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    const int ww = 374, wh = 278;
    g_hwnd = CreateWindowExW(0, L"OpenAnselLauncher",
                             L"OpenAnsel \u2014 Mirror's Edge Launcher",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
                             (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                             ww, wh, nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

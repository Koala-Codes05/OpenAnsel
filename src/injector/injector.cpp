#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <vector>

DWORD FindProcessId(const std::wstring& processName) {
    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (processName == pe.szExeFile) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return pid;
}

bool InjectDll(DWORD processId, const std::wstring& dllPath) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, processId);
    if (!process) {
        std::wcerr << L"Failed to open target process. Error: " << GetLastError() << std::endl;
        return false;
    }

    const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        std::wcerr << L"Failed to allocate memory in target process. Error: " << GetLastError() << std::endl;
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remotePath, dllPath.data(), pathBytes, nullptr)) {
        std::wcerr << L"Failed to write DLL path to target process. Error: " << GetLastError() << std::endl;
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        std::wcerr << L"Failed to get kernel32.dll handle." << std::endl;
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    LPTHREAD_START_ROUTINE loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    if (!loadLibrary) {
        std::wcerr << L"Failed to find LoadLibraryW address." << std::endl;
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (!thread) {
        std::wcerr << L"Failed to create remote thread. Error: " << GetLastError() << std::endl;
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return true;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::wcerr << L"Usage: openansel_injector.exe <target_exe_name> [dll_path]" << std::endl;
        std::wcerr << L"Example: openansel_injector.exe MirrorsEdge.exe openansel_dx9.dll" << std::endl;
        return 1;
    }

    std::wstring targetExe = argv[1];
    std::wstring dllPath;
    if (argc >= 3) {
        dllPath = argv[2];
    } else {
        wchar_t modulePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        dllPath = std::wstring(modulePath);
        const size_t slash = dllPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dllPath = dllPath.substr(0, slash + 1) + L"openansel_dx9.dll";
        } else {
            dllPath = L"openansel_dx9.dll";
        }
    }

    std::wcout << L"Looking for process: " << targetExe << std::endl;
    DWORD pid = FindProcessId(targetExe);
    if (pid == 0) {
        std::wcerr << L"Target process not found." << std::endl;
        return 1;
    }

    std::wcout << L"Found PID: " << pid << std::endl;
    std::wcout << L"Injecting DLL: " << dllPath << std::endl;

    if (InjectDll(pid, dllPath)) {
        std::wcout << L"Injection succeeded." << std::endl;
        return 0;
    }

    std::wcerr << L"Injection failed." << std::endl;
    return 1;
}

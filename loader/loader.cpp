#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "psapi.lib")

static std::string exe_dir(){
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
    std::string path(buf);
    size_t pos = path.rfind('\\');
    return pos != std::string::npos ? path.substr(0, pos) : path;
}

static std::string resolve_dll(const char* override_arg){
    if(override_arg && override_arg[0]) return std::string(override_arg);
    std::string dir = exe_dir();
    const char* candidates[] = {
        "\\..\\..\\..\\bin_out\\x64\\Release\\CRNativeDumper.dll",  // loader\loader\out\ -> root
        "\\..\\..\\bin_out\\x64\\Release\\CRNativeDumper.dll",      // loader\out\ -> root
        "\\..\\bin_out\\x64\\Release\\CRNativeDumper.dll",          // out\ -> root
        "\\bin_out\\x64\\Release\\CRNativeDumper.dll",
        "\\CRNativeDumper.dll",
        "\\..\\CRNativeDumper.dll",
        "\\..\\..\\CRNativeDumper.dll",
        "\\..\\..\\..\\CRNativeDumper.dll",
    };
    for(const char* rel : candidates){
        std::string c = dir + rel;
        char resolved[MAX_PATH] = {};
        if(GetFullPathNameA(c.c_str(), MAX_PATH, resolved, nullptr)){
            DWORD a = GetFileAttributesA(resolved);
            if(a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                return std::string(resolved);
        }
    }
    // fallback: same dir as exe
    return dir + "\\CRNativeDumper.dll";
}

static DWORD find_pid(const wchar_t* name){
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {sizeof(pe)};
    DWORD pid = 0;
    if(Process32FirstW(snap, &pe)) do {
        if(_wcsicmp(pe.szExeFile, name) == 0){ pid = pe.th32ProcessID; break; }
    } while(Process32NextW(snap, &pe));
    CloseHandle(snap);
    return pid;
}

struct WndCtx { DWORD pid; HWND hwnd; };
static BOOL CALLBACK enum_wnd_cb(HWND hwnd, LPARAM lp){
    WndCtx* ctx = (WndCtx*)lp;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if(wpid == ctx->pid && IsWindowVisible(hwnd) && IsWindowEnabled(hwnd)){
        char title[256] = {};
        GetWindowTextA(hwnd, title, sizeof(title));
        if(strlen(title) > 0){
            ctx->hwnd = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static bool process_has_window(DWORD pid){
    WndCtx ctx = {pid, nullptr};
    EnumWindows(enum_wnd_cb, (LPARAM)&ctx);
    return ctx.hwnd != nullptr;
}

static bool inject(DWORD pid, const std::string& dll_path){
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if(!hProc){ fprintf(stderr, "[!] OpenProcess basarisiz: %lu\n", GetLastError()); return false; }

    size_t path_len = dll_path.size() + 1;
    LPVOID remote = VirtualAllocEx(hProc, nullptr, path_len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if(!remote){ CloseHandle(hProc); return false; }
    WriteProcessMemory(hProc, remote, dll_path.c_str(), path_len, nullptr);

    FARPROC loadlib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE hThread  = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)loadlib, remote, 0, nullptr);
    if(!hThread){
        fprintf(stderr, "[!] CreateRemoteThread basarisiz: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    printf("[+] Inject thread olusturuldu — bekleniyor...\n");
    WaitForSingleObject(hThread, 15000);
    DWORD code = 0;
    GetExitCodeThread(hThread, &code);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if(code){
        printf("[+] Inject basarili — DLL handle: 0x%08lX\n", code);
        return true;
    }
    fprintf(stderr, "[!] LoadLibrary basarisiz (handle=0)\n");
    return false;
}

int main(int argc, char* argv[]){
    const char* dll_override = argc > 1 ? argv[1] : nullptr;
    std::string dll_path = resolve_dll(dll_override);

    SetConsoleTitleA("CRNativeDumper Loader -- By cmmdx256");
    printf("--By cmmdx256\n\n");
    printf("[*] DLL  : %s\n", dll_path.c_str());

    DWORD attr = GetFileAttributesA(dll_path.c_str());
    if(attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)){
        fprintf(stderr, "\n[!] DLL bulunamadi:\n    %s\n", dll_path.c_str());
        fprintf(stderr, "\n    Cozum: loader.exe ile ayni klasore CRNativeDumper.dll koy\n");
        fprintf(stderr, "    veya: loader.exe \"C:\\tam\\yol\\CRNativeDumper.dll\"\n\n");
        fprintf(stderr, "Devam etmek icin bir tusa basin...\n");
        getchar();
        return 1;
    }
    printf("[+] DLL  bulundu: %s\n", dll_path.c_str());

    printf("[*] Hedef: craftrise-x64.exe\n");
    printf("[*] Craftrise bekleniyor...\n\n");

    DWORD pid = 0;
    int  wait_tick = 0;
    while(!pid){
        pid = find_pid(L"craftrise-x64.exe");
        if(!pid){
            Sleep(500);
            if(++wait_tick % 10 == 0)
                printf("[~] Bekliyor... (%ds)\n", wait_tick / 2);
        }
    }
    printf("[+] craftrise-x64.exe bulundu — PID: %lu\n", pid);
    printf("[*] Pencere bekleniyor...\n");

    // ── Step 1: wait for window ────────────────────────────────────────────
    int win_tick = 0;
    while(!process_has_window(pid)){
        Sleep(500);
        if(++win_tick % 10 == 0)
            printf("[~] Pencere bekleniyor... (%ds)\n", win_tick / 2);
        if(find_pid(L"craftrise-x64.exe") == 0){ printf("[!] Craftrise kapandi.\n"); return 1; }
    }
    printf("[+] Pencere geldi. 5s bekleniyor...\n");

    // Simple fixed wait after window — game needs time to finish loading JVM classes
    for(int i = 5; i > 0; i--){
        printf("[~] %ds...\n", i);
        Sleep(1000);
        if(find_pid(L"craftrise-x64.exe") == 0){ printf("[!] Craftrise kapandi.\n"); return 1; }
    }
    printf("[+] Inject ediliyor...\n");

    if(!inject(pid, dll_path)) return 1;

    printf("[+] Basarili. CRNativeDumper konsol penceresi acilacak.\n");
    printf("\n--By cmmdx256\n");
    Sleep(1500);
    return 0;
}

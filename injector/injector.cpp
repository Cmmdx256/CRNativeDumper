#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

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

static std::string exe_dir(){
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
    std::string path(buf);
    size_t pos = path.rfind('\\');
    return pos != std::string::npos ? path.substr(0, pos) : path;
}

static std::string resolve_dll(const char* override_arg){
    if(override_arg && override_arg[0]){
        return std::string(override_arg);
    }

    std::string dir = exe_dir();

    const char* relative_candidates[] = {
        "\\..\\bin_out\\x64\\Release\\CRNativeDumper.dll",
        "\\..\\..\\bin_out\\x64\\Release\\CRNativeDumper.dll",
        "\\CRNativeDumper.dll",
        "\\..\\CRNativeDumper.dll",
    };

    for(const char* rel : relative_candidates){
        std::string candidate = dir + rel;
        char resolved[MAX_PATH] = {};
        if(GetFullPathNameA(candidate.c_str(), MAX_PATH, resolved, nullptr)){
            DWORD attr = GetFileAttributesA(resolved);
            if(attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)){
                return std::string(resolved);
            }
        }
    }

    return dir + "\\..\\bin_out\\x64\\Release\\CRNativeDumper.dll";
}

int main(int argc, char* argv[]){
    const char* override_arg = argc > 1 ? argv[1] : nullptr;
    std::string dll_path = resolve_dll(override_arg);
    const wchar_t* proc_name = L"craftrise-x64.exe";

    printf("--By cmmdx256\n\n");
    printf("[*] DLL  : %s\n", dll_path.c_str());
    printf("[*] Hedef: craftrise-x64.exe\n");

    DWORD attr = GetFileAttributesA(dll_path.c_str());
    if(attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)){
        fprintf(stderr, "[!] DLL dosyasi bulunamadi: %s\n", dll_path.c_str());
        fprintf(stderr, "    Kullanim: injector.exe [dll_yolu]\n");
        return 1;
    }

    DWORD pid = find_pid(proc_name);
    if(!pid){ fprintf(stderr, "[!] craftrise-x64.exe bulunamadi\n"); return 1; }
    printf("[+] PID  : %lu\n", pid);

    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
        PROCESS_VM_READ       | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if(!hProc){ fprintf(stderr, "[!] OpenProcess basarisiz: %lu\n", GetLastError()); return 1; }

    size_t path_len = dll_path.size() + 1;
    LPVOID remote_str = VirtualAllocEx(hProc, nullptr, path_len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if(!remote_str){
        fprintf(stderr, "[!] VirtualAllocEx basarisiz\n");
        CloseHandle(hProc); return 1;
    }

    if(!WriteProcessMemory(hProc, remote_str, dll_path.c_str(), path_len, nullptr)){
        fprintf(stderr, "[!] WriteProcessMemory basarisiz\n");
        VirtualFreeEx(hProc, remote_str, 0, MEM_RELEASE); CloseHandle(hProc); return 1;
    }

    HMODULE hK32   = GetModuleHandleA("kernel32.dll");
    FARPROC loadlib = GetProcAddress(hK32, "LoadLibraryA");

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)loadlib, remote_str, 0, nullptr);
    if(!hThread){
        fprintf(stderr, "[!] CreateRemoteThread basarisiz: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remote_str, 0, MEM_RELEASE); CloseHandle(hProc); return 1;
    }

    printf("[+] Inject thread olusturuldu — bekleniyor...\n");
    WaitForSingleObject(hThread, 10000);

    DWORD exit_code = 0;
    GetExitCodeThread(hThread, &exit_code);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote_str, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if(exit_code){
        printf("[+] Inject basarili — DLL handle: 0x%08lX\n", exit_code);
        printf("[+] CRNativeDumper konsol penceresi acilmali.\n");
        return 0;
    } else {
        fprintf(stderr, "[!] LoadLibrary basarisiz (handle=0) — DLL yuklenmedi\n");
        return 1;
    }
}

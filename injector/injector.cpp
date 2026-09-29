
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>

static DWORD find_pid(const wchar_t* name){
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe={sizeof(pe)};
    DWORD pid=0;
    if(Process32FirstW(snap,&pe)) do{
        if(_wcsicmp(pe.szExeFile,name)==0){pid=pe.th32ProcessID;break;}
    }while(Process32NextW(snap,&pe));
    CloseHandle(snap);
    return pid;
}

int main(int argc,char* argv[]){
    
    const char* dll_path = argc>1 ? argv[1] : "C:\\Users\\Theso\\Desktop\\CRNativeDumper\\bin_out\\x64\\Release\\CRNativeDumper.dll";
    const wchar_t* proc_name = L"craftrise-x64.exe";

    printf("[*] DLL: %s\n", dll_path);
    printf("[*] Target: craftrise-x64.exe\n");

    DWORD pid = find_pid(proc_name);
    if(!pid){ fprintf(stderr,"[!] craftrise-x64.exe bulunamadi\n"); return 1; }
    printf("[+] PID: %lu\n", pid);

    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ|PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if(!hProc){ fprintf(stderr,"[!] OpenProcess basarisiz: %lu\n",GetLastError()); return 1; }

    size_t path_len = strlen(dll_path)+1;
    LPVOID remote_str = VirtualAllocEx(hProc, nullptr, path_len, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(!remote_str){ fprintf(stderr,"[!] VirtualAllocEx basarisiz\n"); CloseHandle(hProc); return 1; }

    if(!WriteProcessMemory(hProc, remote_str, dll_path, path_len, nullptr)){
        fprintf(stderr,"[!] WriteProcessMemory basarisiz\n");
        VirtualFreeEx(hProc,remote_str,0,MEM_RELEASE); CloseHandle(hProc); return 1;
    }

    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadlib = GetProcAddress(hK32,"LoadLibraryA");

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)loadlib, remote_str, 0, nullptr);
    if(!hThread){
        fprintf(stderr,"[!] CreateRemoteThread basarisiz: %lu\n",GetLastError());
        VirtualFreeEx(hProc,remote_str,0,MEM_RELEASE); CloseHandle(hProc); return 1;
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
        fprintf(stderr,"[!] LoadLibrary basarisiz (handle=0) — DLL yuklenmedi\n");
        return 1;
    }
}

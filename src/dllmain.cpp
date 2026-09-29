
#include <Windows.h>
#include "dumper.h"

static DWORD WINAPI worker_thread(LPVOID) {
    run_dumper();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, worker_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}

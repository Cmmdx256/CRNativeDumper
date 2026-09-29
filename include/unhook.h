// language: C++, file: unhook.h, runtime: Windows x64 MSVC
// PintoBounce pattern: vtable pointer'ının başındaki JMP zincirini atlayıp
// ham native adrese ulaşır. Her JNI/JVMTI çağrısı buradan geçmeli.
#pragma once
#include <Windows.h>
#include <cstdint>

// JMP rel32 ve JMP [rip+0] zincirini takip ederek gerçek fonksiyon adresine ulaşır.
template<typename T>
inline T unhook(T ptr) {
    auto p = reinterpret_cast<uint8_t*>(ptr);
    for (int i = 0; i < 8; i++) {
        if (p[0] == 0xE9) {
            // JMP rel32
            int32_t rel = *reinterpret_cast<int32_t*>(p + 1);
            p = p + 5 + rel;
        } else if (p[0] == 0xFF && p[1] == 0x25) {
            // JMP [rip+disp32]
            int32_t disp = *reinterpret_cast<int32_t*>(p + 2);
            uintptr_t target_addr = reinterpret_cast<uintptr_t>(p + 6 + disp);
            p = reinterpret_cast<uint8_t*>(*reinterpret_cast<uintptr_t*>(target_addr));
        } else {
            break;
        }
    }
    return reinterpret_cast<T>(p);
}

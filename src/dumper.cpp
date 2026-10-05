
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include "dumper.h"
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <gl/GL.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <share.h>
#include <string>
#include <mutex>
#include <set>
#include <atomic>
#include <vector>
#include <map>
#include "../include/jni.h"
#include "../include/jvmti.h"
#include "../include/MinHook.h"
#include "../include/unhook.h"

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "psapi.lib")

typedef jclass   (JNICALL* tFindClass)            (JNIEnv*, const char*);
typedef jmethodID(JNICALL* tGetMethodID)           (JNIEnv*, jclass, const char*, const char*);
typedef jobject  (JNICALL* tCallObjectMethod)      (JNIEnv*, jobject, jmethodID, ...);
typedef const char*(JNICALL* tGetStringUTFChars)   (JNIEnv*, jstring, jboolean*);
typedef void     (JNICALL* tReleaseStringUTFChars) (JNIEnv*, jstring, const char*);
typedef jobject  (JNICALL* tNewGlobalRef)          (JNIEnv*, jobject);
typedef void     (JNICALL* tDeleteLocalRef)        (JNIEnv*, jobject);
typedef jboolean (JNICALL* tExceptionCheck)        (JNIEnv*);
typedef void     (JNICALL* tExceptionClear)        (JNIEnv*);

typedef jint(JNICALL* tInternalGetEnv)(JavaVM*, void**, jint);
typedef LONG(NTAPI* pNtQIT)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static JavaVM*    g_jvm    = nullptr;
static JNIEnv*    g_env    = nullptr;
static jvmtiEnv*  g_jvmti  = nullptr;
static MODULEINFO g_jvm_mi = {};
static pNtQIT     g_NtQIT  = nullptr;

static tFindClass             raw_FindClass             = nullptr;
static tGetMethodID           raw_GetMethodID           = nullptr;
static tCallObjectMethod      raw_CallObjectMethod      = nullptr;
static tGetStringUTFChars     raw_GetStringUTFChars     = nullptr;
static tReleaseStringUTFChars raw_ReleaseStringUTFChars = nullptr;
static tNewGlobalRef          raw_NewGlobalRef          = nullptr;
static tDeleteLocalRef        raw_DeleteLocalRef        = nullptr;
static tExceptionCheck        raw_ExceptionCheck        = nullptr;
static tExceptionClear        raw_ExceptionClear        = nullptr;

static jclass    g_jcls_Class   = nullptr;
static jmethodID g_jmid_getName = nullptr;
static jmethodID g_jmid_getDeclaredFields  = nullptr;
static jmethodID g_jmid_getDeclaredMethods = nullptr;
static jclass    g_jcls_Field   = nullptr;
static jmethodID g_jmid_fldName = nullptr;
static jmethodID g_jmid_fldType = nullptr;
static jclass    g_jcls_Method  = nullptr;
static jmethodID g_jmid_mthName = nullptr;

static jmethodID g_jmid_fldMods      = nullptr;
static jmethodID g_jmid_classTypeName= nullptr;
static jmethodID g_jmid_mthRet       = nullptr;
static jmethodID g_jmid_mthParams    = nullptr;
static jmethodID g_jmid_mthMods      = nullptr;
static jobject   g_unsafe            = nullptr;
static jmethodID g_jmid_unsafeOFO   = nullptr;
static jmethodID g_jmid_unsafeSFO   = nullptr;

static jmethodID g_jmid_classMods        = nullptr;
static jmethodID g_jmid_getSuperclass    = nullptr;
static jmethodID g_jmid_getInterfaces    = nullptr;
static jmethodID g_jmid_isInterface      = nullptr;
static jmethodID g_jmid_isEnum           = nullptr;
static jmethodID g_jmid_isAnnotation     = nullptr;
static jmethodID g_jmid_getAnnotations   = nullptr;
static jmethodID g_jmid_annoTypeName     = nullptr;
static jmethodID g_jmid_mthExcTypes      = nullptr;
static jmethodID g_jmid_getDeclaredCtors = nullptr;
static jclass    g_jcls_Constructor      = nullptr;
static jmethodID g_jmid_ctorParams       = nullptr;
static jmethodID g_jmid_ctorMods         = nullptr;

static FILE*  g_file     = nullptr;
static FILE*  g_fmt_file = nullptr;   
static FILE*  g_net_file    = nullptr;
static FILE*  g_schema_file = nullptr;
static FILE*  g_crypto_file = nullptr;
static HANDLE g_con  = INVALID_HANDLE_VALUE;
static std::mutex            g_mutex;
static std::set<std::string> g_written;
static std::map<std::string,std::string> g_source_names;
static std::atomic<int>      g_class_count{ 0 };

typedef void(__stdcall* wglSwapBuffers_t)(HDC);
static wglSwapBuffers_t  orig_wglSwapBuffers = nullptr;
static std::atomic<bool> g_gl_ready{ false };
static std::atomic<bool> g_env_ready{ false };
static std::atomic<bool> g_dump_done{ false };
static std::atomic<bool> g_class_dump_mode{ false };

static const uintptr_t JDK8U51_GET_ENV_OFFSET = 0x144080;

static char g_outdir[MAX_PATH]       = {};
static char g_class_dump_dir[MAX_PATH] = {};

static void make_output_dir(){
    const char* dir_c = "C:\\CRNativeDumperByCmmdx256";
    if(CreateDirectoryA(dir_c, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS){
        _snprintf_s(g_outdir, sizeof(g_outdir), "%s", dir_c);
        _snprintf_s(g_class_dump_dir, sizeof(g_class_dump_dir), "%s\\craftrisedumped", dir_c);
        CreateDirectoryA(g_class_dump_dir, nullptr);
        return;
    }
    char prof[MAX_PATH] = {};
    if(GetEnvironmentVariableA("USERPROFILE", prof, sizeof(prof))){
        char dp[MAX_PATH];
        _snprintf_s(dp, sizeof(dp), "%.220s\\Desktop\\CRNativeDumperByCmmdx256", prof);
        if(CreateDirectoryA(dp, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS){
            _snprintf_s(g_outdir, sizeof(g_outdir), "%s", dp);
            _snprintf_s(g_class_dump_dir, sizeof(g_class_dump_dir), "%s\\craftrisedumped", dp);
            CreateDirectoryA(g_class_dump_dir, nullptr);
            return;
        }
        _snprintf_s(g_outdir, sizeof(g_outdir), "%.240s\\Desktop", prof);
        _snprintf_s(g_class_dump_dir, sizeof(g_class_dump_dir), "%.232s\\Desktop\\craftrisedumped", prof);
        CreateDirectoryA(g_class_dump_dir, nullptr);
    }
}

static void make_dirs_for(const char* filepath){
    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), "%s", filepath);
    for(char* p = tmp + 1; *p; p++){
        if(*p == '\\'){
            *p = '\0';
            CreateDirectoryA(tmp, nullptr);
            *p = '\\';
        }
    }
}

// ── Wide-char path helpers (for Unicode obfuscated class names) ─────────────
static std::wstring utf8_to_wide(const std::string& s){
    if(s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if(n <= 0) return L"";
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static void make_dirs_for_w(const std::wstring& wpath){
    std::wstring tmp = wpath;
    for(size_t i = 1; i < tmp.size(); i++){
        if(tmp[i] == L'\\'){
            tmp[i] = L'\0';
            CreateDirectoryW(tmp.c_str(), nullptr);
            tmp[i] = L'\\';
        }
    }
}

static FILE* wfopen_write(const std::string& utf8_path){
    std::wstring wp = utf8_to_wide(utf8_path);
    // Ensure parent dirs exist
    make_dirs_for_w(wp);
    return _wfopen(wp.c_str(), L"wb");
}

static bool wfile_exists(const std::string& utf8_path){
    std::wstring wp = utf8_to_wide(utf8_path);
    DWORD a = GetFileAttributesW(wp.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
}


enum Col : WORD {
    WHITE  = FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE|FOREGROUND_INTENSITY,
    CYAN   = FOREGROUND_GREEN|FOREGROUND_BLUE|FOREGROUND_INTENSITY,
    YELLOW = FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_INTENSITY,
    GREEN  = FOREGROUND_GREEN|FOREGROUND_INTENSITY,
    RED    = FOREGROUND_RED|FOREGROUND_INTENSITY,
    GREY   = FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE,
};
static void set_col(WORD c){ if(g_con!=INVALID_HANDLE_VALUE) SetConsoleTextAttribute(g_con,c); }

static FILE* g_dbg = nullptr;

static void con_log(WORD c, const char* fmt, ...){
    std::lock_guard<std::mutex> lk(g_mutex);
    set_col(c);
    va_list a; va_start(a,fmt); vprintf(fmt,a); va_end(a);
    set_col(WHITE);
    if(g_dbg){
        va_list b; va_start(b,fmt); vfprintf(g_dbg,fmt,b); va_end(b);
        fflush(g_dbg);
    }
}

static bool is_in_jvm(uintptr_t p){
    uintptr_t base=(uintptr_t)g_jvm_mi.lpBaseOfDll;
    return base && p>=base && p<base+g_jvm_mi.SizeOfImage;
}
static bool looks_like_jni_functions(uintptr_t p){
    if(!is_in_jvm(p)) return false;
    __try{
        uintptr_t* t=(uintptr_t*)p;
        for(int i=3;i<=7;i++) if(!is_in_jvm(t[i])) return false;
        return true;
    }__except(1){return false;}
}

static void init_jni_pointers(JNIEnv* env){
    void** vt = *reinterpret_cast<void***>(env);
    raw_FindClass             = (tFindClass)            vt[6];
    raw_ExceptionClear        = (tExceptionClear)       vt[17];   
    raw_NewGlobalRef          = (tNewGlobalRef)         vt[21];
    raw_DeleteLocalRef        = (tDeleteLocalRef)       vt[23];
    raw_GetMethodID           = (tGetMethodID)          vt[33];
    raw_CallObjectMethod      = (tCallObjectMethod)     vt[34];
    raw_GetStringUTFChars     = (tGetStringUTFChars)    vt[169];
    raw_ReleaseStringUTFChars = (tReleaseStringUTFChars)vt[170];
    raw_ExceptionCheck        = (tExceptionCheck)       vt[228];
}

static bool init_jni_reflection(JNIEnv* env){
    
    jclass raw = raw_FindClass(env, "java/lang/Class");
    if(!raw || raw_ExceptionCheck(env)){ raw_ExceptionClear(env); return false; }
    g_jcls_Class  = (jclass)raw_NewGlobalRef(env, raw);
    raw_DeleteLocalRef(env, raw);
    g_jmid_getName            = raw_GetMethodID(env, g_jcls_Class, "getName",            "()Ljava/lang/String;");
    g_jmid_getDeclaredFields  = raw_GetMethodID(env, g_jcls_Class, "getDeclaredFields",  "()[Ljava/lang/reflect/Field;");
    g_jmid_getDeclaredMethods = raw_GetMethodID(env, g_jcls_Class, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    raw_ExceptionClear(env);
    if(!g_jmid_getName) return false;

    raw = raw_FindClass(env, "java/lang/reflect/Field");
    if(raw && !raw_ExceptionCheck(env)){
        g_jcls_Field = (jclass)raw_NewGlobalRef(env, raw);
        raw_DeleteLocalRef(env, raw);
        g_jmid_fldName = raw_GetMethodID(env, g_jcls_Field, "getName",    "()Ljava/lang/String;");
        g_jmid_fldType = raw_GetMethodID(env, g_jcls_Field, "getType",    "()Ljava/lang/Class;");
    }
    raw_ExceptionClear(env);

    raw = raw_FindClass(env, "java/lang/reflect/Method");
    if(raw && !raw_ExceptionCheck(env)){
        g_jcls_Method = (jclass)raw_NewGlobalRef(env, raw);
        raw_DeleteLocalRef(env, raw);
        g_jmid_mthName = raw_GetMethodID(env, g_jcls_Method, "getName", "()Ljava/lang/String;");
    }
    raw_ExceptionClear(env);

    if(g_jcls_Field){
        g_jmid_fldMods = raw_GetMethodID(env, g_jcls_Field, "getModifiers", "()I");
        raw_ExceptionClear(env);
    }
    
    if(g_jcls_Class){
        g_jmid_classTypeName = raw_GetMethodID(env, g_jcls_Class, "getTypeName", "()Ljava/lang/String;");
        raw_ExceptionClear(env);
    }
    
    if(g_jcls_Method){
        g_jmid_mthRet    = raw_GetMethodID(env, g_jcls_Method, "getReturnType",     "()Ljava/lang/Class;");
        g_jmid_mthParams = raw_GetMethodID(env, g_jcls_Method, "getParameterTypes", "()[Ljava/lang/Class;");
        g_jmid_mthMods   = raw_GetMethodID(env, g_jcls_Method, "getModifiers",      "()I");
        raw_ExceptionClear(env);
    }
    
    {
        jclass cls_u = raw_FindClass(env, "sun/misc/Unsafe");
        raw_ExceptionClear(env);
        if(cls_u){
            jfieldID fid = env->GetStaticFieldID(cls_u, "theUnsafe", "Lsun/misc/Unsafe;");
            raw_ExceptionClear(env);
            if(fid){
                jobject ul = env->GetStaticObjectField(cls_u, fid);
                raw_ExceptionClear(env);
                if(ul){ g_unsafe = raw_NewGlobalRef(env, ul); raw_DeleteLocalRef(env, ul); }
            }
            g_jmid_unsafeOFO = raw_GetMethodID(env, cls_u, "objectFieldOffset", "(Ljava/lang/reflect/Field;)J");
            g_jmid_unsafeSFO = raw_GetMethodID(env, cls_u, "staticFieldOffset",  "(Ljava/lang/reflect/Field;)J");
            raw_ExceptionClear(env);
            raw_DeleteLocalRef(env, cls_u);
        }
        con_log(g_unsafe ? GREEN : YELLOW, "[%s] sun.misc.Unsafe: %s\n",
            g_unsafe ? "+" : "!", g_unsafe ? "OK" : "FAIL (offsets disabled)");
    }

    if(g_jcls_Class){
        g_jmid_classMods     = raw_GetMethodID(env, g_jcls_Class, "getModifiers",    "()I");
        g_jmid_getSuperclass = raw_GetMethodID(env, g_jcls_Class, "getSuperclass",   "()Ljava/lang/Class;");
        g_jmid_getInterfaces = raw_GetMethodID(env, g_jcls_Class, "getInterfaces",   "()[Ljava/lang/Class;");
        g_jmid_isInterface   = raw_GetMethodID(env, g_jcls_Class, "isInterface",     "()Z");
        g_jmid_isEnum        = raw_GetMethodID(env, g_jcls_Class, "isEnum",          "()Z");
        g_jmid_isAnnotation  = raw_GetMethodID(env, g_jcls_Class, "isAnnotation",    "()Z");
        g_jmid_getAnnotations= raw_GetMethodID(env, g_jcls_Class, "getDeclaredAnnotations", "()[Ljava/lang/annotation/Annotation;");
        g_jmid_getDeclaredCtors = raw_GetMethodID(env, g_jcls_Class, "getDeclaredConstructors", "()[Ljava/lang/reflect/Constructor;");
        raw_ExceptionClear(env);
    }
    if(g_jcls_Method){
        g_jmid_mthExcTypes = raw_GetMethodID(env, g_jcls_Method, "getExceptionTypes", "()[Ljava/lang/Class;");
        raw_ExceptionClear(env);
    }
    {
        jclass cls_anno = raw_FindClass(env, "java/lang/annotation/Annotation");
        raw_ExceptionClear(env);
        if(cls_anno){
            g_jmid_annoTypeName = raw_GetMethodID(env, cls_anno, "annotationType", "()Ljava/lang/Class;");
            raw_ExceptionClear(env);
            raw_DeleteLocalRef(env, cls_anno);
        }
    }
    {
        jclass cls_ctor = raw_FindClass(env, "java/lang/reflect/Constructor");
        raw_ExceptionClear(env);
        if(cls_ctor){
            g_jcls_Constructor = (jclass)raw_NewGlobalRef(env, cls_ctor);
            raw_DeleteLocalRef(env, cls_ctor);
            g_jmid_ctorParams = raw_GetMethodID(env, g_jcls_Constructor, "getParameterTypes", "()[Ljava/lang/Class;");
            g_jmid_ctorMods   = raw_GetMethodID(env, g_jcls_Constructor, "getModifiers",      "()I");
            raw_ExceptionClear(env);
        }
    }

    return true;
}

static std::string jstr_to_std(JNIEnv* env, jstring js){
    if(!js) return {};
    const char* u = raw_GetStringUTFChars(env, js, nullptr);
    if(!u){ raw_ExceptionClear(env); return {}; }
    std::string r(u);
    raw_ReleaseStringUTFChars(env, js, u);
    return r;
}

static std::string get_type_name(JNIEnv* env, jobject cls){
    if(!cls) return "?";
    
    if(g_jmid_classTypeName){
        jobject ts = raw_CallObjectMethod(env, cls, g_jmid_classTypeName);
        raw_ExceptionClear(env);
        if(ts){ std::string r = jstr_to_std(env, (jstring)ts); raw_DeleteLocalRef(env, ts); if(!r.empty()) return r; }
    }
    
    if(g_jmid_getName){
        jobject ts = raw_CallObjectMethod(env, cls, g_jmid_getName);
        raw_ExceptionClear(env);
        if(ts){ std::string r = jstr_to_std(env, (jstring)ts); raw_DeleteLocalRef(env, ts); if(!r.empty()) return r; }
    }
    return "?";
}


static std::string str_contains(const std::string& s, const char* sub){
    return s.find(sub) != std::string::npos ? sub : "";
}
static bool icontains(const std::string& s, const char* sub){
    std::string lo = s, lsub = sub;
    for(auto& c:lo)  c=(char)tolower((unsigned char)c);
    for(auto& c:lsub)c=(char)tolower((unsigned char)c);
    return lo.find(lsub) != std::string::npos;
}

static std::string decode_access(jint m){
    if(m & 0x1) return "public";
    if(m & 0x2) return "private";
    if(m & 0x4) return "protected";
    return "package";
}

static std::string field_semantic_tags(const std::string& name, const std::string& type, jint mods){
    std::string t;
    if(mods & 0x10) t += " [final]";
    if(mods & 0x40) t += " [volatile]";
    if(mods & 0x80) t += " [transient]";
    if(icontains(name,"posX")||icontains(name,"posY")||icontains(name,"posZ")||name=="x"||name=="y"||name=="z")
        t += " [position]";
    else if(icontains(name,"yaw")||icontains(name,"pitch")||icontains(name,"rotation"))
        t += " [rotation]";
    else if(icontains(name,"health")||icontains(name,"maxHealth"))
        t += " [health]";
    else if(icontains(name,"speed")||icontains(name,"velocity"))
        t += " [velocity]";
    else if(icontains(name,"width")||icontains(name,"height")||icontains(name,"depth"))
        t += " [dimensions]";
    else if(type=="boolean"||type=="java.lang.Boolean")
        t += " [flag]";
    if(icontains(type,"List")||icontains(type,"ArrayList")||icontains(type,"Set"))
        t += " [collection]";
    if(icontains(type,"Map")||icontains(type,"HashMap")||icontains(type,"ConcurrentHashMap"))
        t += " [map]";
    if(icontains(type,"String")) t += " [string]";
    if(icontains(type,"Thread")) t += " [thread-ref]";
    if(icontains(name,"render")||icontains(name,"texture")||icontains(name,"shader"))
        t += " [render-data]";
    if(icontains(name,"entity")||icontains(name,"player")||icontains(name,"mob"))
        t += " [entity-ref]";
    if(icontains(name,"world")||icontains(name,"chunk")||icontains(name,"block"))
        t += " [world-ref]";
    if(icontains(name,"packet")||icontains(name,"socket")||icontains(name,"network"))
        t += " [network-ref]";
    if(icontains(name,"timer")||icontains(name,"tick")||icontains(name,"counter"))
        t += " [timer]";
    if(icontains(name,"debug")||icontains(name,"log")||icontains(name,"trace"))
        t += " [debug]";
    if(name=="INSTANCE"||icontains(name,"instance")||name=="theMinecraft"||name=="mc")
        t += " [singleton]";
    return t;
}

static std::string method_semantic_tags(const std::string& name, const std::string& ret, jint mods){
    std::string t;
    if(mods & 0x400) t += " [abstract]";
    if(mods & 0x100) t += " [native]";
    if(mods & 0x020) t += " [synchronized]";
    if(mods & 0x010) t += " [final]";
    size_t nl = name.size();
    const char* n = name.c_str();
    if(nl>3 && name.substr(0,3)=="get" && isupper((unsigned char)n[3])) t += " [getter]";
    else if(nl>3 && name.substr(0,3)=="set" && isupper((unsigned char)n[3])) t += " [setter]";
    else if(nl>2 && name.substr(0,2)=="is"  && isupper((unsigned char)n[2])) t += " [bool-check]";
    else if(nl>3 && name.substr(0,3)=="has" && isupper((unsigned char)n[3])) t += " [bool-check]";
    else if(nl>2 && name.substr(0,2)=="on"  && isupper((unsigned char)n[2])) t += " [event-handler]";
    if(icontains(name,"tick")||icontains(name,"update")||name=="onUpdate"||name=="onLivingUpdate") t += " [tick]";
    if(icontains(name,"render")||icontains(name,"draw")||icontains(name,"paint")) t += " [render]";
    if(icontains(name,"init")||icontains(name,"setup")||icontains(name,"load")||icontains(name,"start")) t += " [init]";
    if(icontains(name,"destroy")||icontains(name,"cleanup")||icontains(name,"dispose")||icontains(name,"close")) t += " [cleanup]";
    if(icontains(name,"send")||icontains(name,"receive")||icontains(name,"packet")||icontains(name,"network")) t += " [network]";
    if(icontains(name,"attack")||icontains(name,"damage")||icontains(name,"kill")||icontains(name,"hurt")) t += " [combat]";
    if(icontains(name,"move")||icontains(name,"jump")||icontains(name,"fly")||icontains(name,"swim")) t += " [movement]";
    if(icontains(name,"spawn")||icontains(name,"create")) t += " [factory]";
    if(ret=="void" && nl<=2 && nl>0) t += " [obf-void]";
    return t;
}

static std::string class_semantic_tags(const std::string& simple, const std::string& full,
                                       const std::string& super, bool is_iface, bool is_enum){
    if(is_enum)  return " [enum]";
    if(is_iface) return " [interface]";
    std::string t;
    if(icontains(simple,"player")||icontains(full,"player")) t += " [player]";
    if(icontains(simple,"entity")||icontains(super,"entity")) t += " [entity]";
    if(icontains(simple,"render")||icontains(full,"render")||icontains(super,"render")) t += " [renderer]";
    if(icontains(simple,"gui")||icontains(simple,"screen")||icontains(super,"gui")||icontains(super,"screen")) t += " [gui]";
    if(icontains(simple,"packet")||icontains(full,"packet")) t += " [network]";
    if(icontains(simple,"manager")||icontains(simple,"registry")) t += " [manager]";
    if(icontains(simple,"world")||icontains(super,"world")) t += " [world]";
    if(icontains(simple,"block")||icontains(super,"block")) t += " [block]";
    if(icontains(simple,"item")||icontains(super,"item")) t += " [item]";
    if(icontains(simple,"event")||icontains(simple,"handler")) t += " [event]";
    if(icontains(simple,"thread")||icontains(super,"thread")||icontains(super,"runnable")) t += " [thread]";
    if(icontains(simple,"exception")||icontains(super,"exception")||icontains(super,"error")) t += " [exception]";
    if(icontains(simple,"inventory")||icontains(super,"inventory")) t += " [inventory]";
    if(icontains(simple,"chunk")||icontains(super,"chunk")) t += " [chunk]";
    if(icontains(simple,"sound")||icontains(simple,"audio")) t += " [audio]";
    if(icontains(simple,"shader")||icontains(simple,"texture")) t += " [render-resource]";
    if(icontains(simple,"ability")||icontains(simple,"skill")||icontains(simple,"effect")) t += " [gameplay]";
    if(icontains(simple,"config")||icontains(simple,"setting")||icontains(simple,"option")) t += " [config]";
    if(icontains(simple,"util")||icontains(simple,"helper")) t += " [utility]";
    return t;
}

static std::string get_class_array_names(JNIEnv* env, jobject arr){
    if(!arr) return "";
    std::string out;
    jint len = env->GetArrayLength((jarray)arr);
    for(jint i = 0; i < len; i++){
        jobject cls = env->GetObjectArrayElement((jobjectArray)arr, i);
        raw_ExceptionClear(env);
        if(!cls) continue;
        if(!out.empty()) out += ", ";
        out += get_type_name(env, cls);
        raw_DeleteLocalRef(env, cls);
    }
    return out;
}

static void write_crypto_entry(const std::string& class_path, const std::vector<std::string>& strings, jclass klass);

static std::string desc_to_human(const std::string& desc){
    auto jtype = [](const char* p, size_t& i) -> std::string {
        switch(p[i]){
            case 'B': return "byte";
            case 'C': return "char";
            case 'D': return "double";
            case 'F': return "float";
            case 'I': return "int";
            case 'J': return "long";
            case 'S': return "short";
            case 'V': return "void";
            case 'Z': return "boolean";
            case '[': { i++; std::string inner; /* handled below */ return inner; }
            case 'L': {
                size_t s = i+1;
                while(i < 512 && p[i] != ';') i++;
                std::string cls(p+s, i-s);
                for(char& c: cls) if(c=='/') c='.';
                return cls;
            }
            default: return std::string(1,p[i]);
        }
    };
    if(desc.empty() || desc[0] != '(') return desc;
    std::string out = "(";
    size_t i = 1; bool first = true;
    const char* p = desc.c_str();
    while(p[i] && p[i] != ')'){
        if(!first) out += ", ";
        first = false;
        int arr = 0;
        while(p[i] == '['){arr++; i++;}
        std::string tp;
        if(p[i]=='L'){ size_t s=i+1; while(p[i] && p[i]!=';') i++; tp=std::string(p+s,i-s); for(char&c:tp) if(c=='/') c='.'; }
        else { switch(p[i]){ case 'B':tp="byte";break; case 'C':tp="char";break; case 'D':tp="double";break; case 'F':tp="float";break; case 'I':tp="int";break; case 'J':tp="long";break; case 'S':tp="short";break; case 'Z':tp="boolean";break; default: tp=std::string(1,p[i]); } }
        for(int a=0;a<arr;a++) tp+="[]";
        out += tp;
        i++;
    }
    out += ")";
    if(p[i]==')') i++;
    std::string ret;
    if(p[i]=='L'){ size_t s=i+1; i++; while(p[i] && p[i]!=';') i++; ret=std::string(p+s,i-s); for(char&c:ret) if(c=='/') c='.'; }
    else { switch(p[i]){ case 'B':ret="byte";break; case 'C':ret="char";break; case 'D':ret="double";break; case 'F':ret="float";break; case 'I':ret="int";break; case 'J':ret="long";break; case 'S':ret="short";break; case 'V':ret="void";break; case 'Z':ret="boolean";break; default: ret=std::string(1,p[i]); } }
    out += " -> " + ret;
    return out;
}

static std::string json_escape(const std::string& s){
    std::string r; r.reserve(s.size()+4);
    for(unsigned char c : s){
        if(c == '"') r += "\\\"";
        else if(c == '\\') r += "\\\\";
        else if(c == '\n') r += "\\n";
        else if(c == '\r') r += "\\r";
        else if(c == '\t') r += "\\t";
        else if(c < 0x20){ char buf[8]; sprintf_s(buf,"\\u%04x",(int)c); r+=buf; }
        else r += c;
    }
    return r;
}

static void dump_class_schema(JNIEnv* env, jclass klass, const std::string& dotname){
    if(!g_schema_file) return;

    // ── source file name ───────────────────────────────────────────────────
    std::string src_name;
    if(g_source_names.count(dotname)) src_name = g_source_names[dotname];
    if(src_name.empty() && g_jvmti){
        char* sf = nullptr;
        if(g_jvmti->GetSourceFileName(klass, &sf) == JVMTI_ERROR_NONE && sf){
            src_name = sf;
            if(src_name.size()>5 && src_name.substr(src_name.size()-5)==".java")
                src_name = src_name.substr(0,src_name.size()-5);
            g_jvmti->Deallocate((unsigned char*)sf);
        }
    }

    // ── kind / super / tags ────────────────────────────────────────────────
    std::string kind_str, tags_str, super_str;
    if(g_jmid_isInterface && env->CallBooleanMethod(klass, g_jmid_isInterface)) kind_str = "INTERFACE";
    else if(g_jmid_isEnum && env->CallBooleanMethod(klass, g_jmid_isEnum)) kind_str = "ENUM";
    else kind_str = "CLASS";
    env->ExceptionClear();
    {
        std::string lo = dotname;
        for(char& c : lo) c = (char)tolower((unsigned char)c);
        const char* nk[] = {"network","packet","entity","render","gui","manager","world","block","item","player","chat","util",nullptr};
        for(int k=0; nk[k]; k++) if(lo.find(nk[k])!=std::string::npos){
            if(!tags_str.empty()) tags_str += " ";
            tags_str += "["; tags_str += nk[k]; tags_str += "]";
        }
    }
    if(g_jmid_getSuperclass){
        jobject sc = env->CallObjectMethod(klass, g_jmid_getSuperclass);
        env->ExceptionClear();
        if(sc){ super_str = get_type_name(env, sc); env->DeleteLocalRef(sc); }
    }

    // ── header ─────────────────────────────────────────────────────────────
    fprintf(g_schema_file, "─────────────────────────────────────────────────────────\n");
    fprintf(g_schema_file, "%s  %s\n", kind_str.c_str(), dotname.c_str());
    if(!src_name.empty())
        fprintf(g_schema_file, "  source  : %s\n", src_name.c_str());
    if(!super_str.empty() && super_str != "java.lang.Object")
        fprintf(g_schema_file, "  extends : %s\n", super_str.c_str());
    if(!tags_str.empty())
        fprintf(g_schema_file, "  tags    : %s\n", tags_str.c_str());

    // ── fields via JNI reflection ──────────────────────────────────────────
    fprintf(g_schema_file, "  FIELDS\n");
    if(g_jmid_getDeclaredFields){
        jobject farr = env->CallObjectMethod(klass, g_jmid_getDeclaredFields);
        env->ExceptionClear();
        if(farr){
            jint flen = env->GetArrayLength((jarray)farr); env->ExceptionClear();
            for(jint i=0;i<flen;i++){
                jobject fobj = env->GetObjectArrayElement((jobjectArray)farr, i);
                env->ExceptionClear();
                if(!fobj) continue;
                jstring fname_js = nullptr;
                if(g_jmid_fldName) fname_js=(jstring)env->CallObjectMethod(fobj,g_jmid_fldName);
                env->ExceptionClear();
                jstring ftype_js = nullptr;
                if(g_jmid_fldType){
                    jobject ft = env->CallObjectMethod(fobj,g_jmid_fldType);
                    env->ExceptionClear();
                    if(ft && g_jmid_getName) ftype_js=(jstring)env->CallObjectMethod(ft,g_jmid_getName);
                    env->ExceptionClear();
                    if(ft) env->DeleteLocalRef(ft);
                }
                std::string fn = fname_js ? jstr_to_std(env,fname_js) : "?";
                std::string ft2 = ftype_js ? jstr_to_std(env,ftype_js) : "?";
                fprintf(g_schema_file, "    %-32s  %s\n", fn.c_str(), ft2.c_str());
                if(fname_js) env->DeleteLocalRef(fname_js);
                if(ftype_js) env->DeleteLocalRef(ftype_js);
                env->DeleteLocalRef(fobj);
            }
            env->DeleteLocalRef(farr);
        }
    }

    // ── methods via JNI reflection ─────────────────────────────────────────
    fprintf(g_schema_file, "  METHODS\n");
    if(g_jmid_getDeclaredMethods){
        jobject marr = env->CallObjectMethod(klass, g_jmid_getDeclaredMethods);
        env->ExceptionClear();
        if(marr){
            jint mlen = env->GetArrayLength((jarray)marr); env->ExceptionClear();
            for(jint i=0;i<mlen;i++){
                jobject mobj = env->GetObjectArrayElement((jobjectArray)marr, i);
                env->ExceptionClear();
                if(!mobj) continue;
                jstring mname_js = nullptr;
                if(g_jmid_mthName) mname_js=(jstring)env->CallObjectMethod(mobj,g_jmid_mthName);
                env->ExceptionClear();
                jstring rtype_js = nullptr;
                if(g_jmid_mthRet){
                    jobject rt = env->CallObjectMethod(mobj,g_jmid_mthRet);
                    env->ExceptionClear();
                    if(rt && g_jmid_getName) rtype_js=(jstring)env->CallObjectMethod(rt,g_jmid_getName);
                    env->ExceptionClear();
                    if(rt) env->DeleteLocalRef(rt);
                }
                std::string params;
                if(g_jmid_mthParams){
                    jobject ptarr = env->CallObjectMethod(mobj,g_jmid_mthParams);
                    env->ExceptionClear();
                    if(ptarr){
                        jint plen = env->GetArrayLength((jarray)ptarr); env->ExceptionClear();
                        for(jint j=0;j<plen;j++){
                            jobject pt = env->GetObjectArrayElement((jobjectArray)ptarr,j);
                            env->ExceptionClear();
                            if(pt){
                                if(!params.empty()) params += ", ";
                                if(g_jmid_getName){
                                    jstring pn=(jstring)env->CallObjectMethod(pt,g_jmid_getName);
                                    env->ExceptionClear();
                                    if(pn){ params+=jstr_to_std(env,pn); env->DeleteLocalRef(pn); }
                                }
                                env->DeleteLocalRef(pt);
                            }
                        }
                        env->DeleteLocalRef(ptarr);
                    }
                }
                std::string mn = mname_js ? jstr_to_std(env,mname_js) : "?";
                std::string rt = rtype_js ? jstr_to_std(env,rtype_js) : "?";
                fprintf(g_schema_file, "    %s(%s)  ->  %s\n", mn.c_str(), params.c_str(), rt.c_str());
                if(mname_js) env->DeleteLocalRef(mname_js);
                if(rtype_js) env->DeleteLocalRef(rtype_js);
                env->DeleteLocalRef(mobj);
            }
            env->DeleteLocalRef(marr);
        }
    }
    fprintf(g_schema_file, "\n");
    fflush(g_schema_file);
}


static void dump_class_fmt(JNIEnv* env, jclass klass, const std::string& dotname){
    if(!g_fmt_file) return;

    std::string simple_name = dotname;
    { size_t p = dotname.rfind('.'); if(p != std::string::npos) simple_name = dotname.substr(p+1); }

    jint cls_mods = 0;
    bool is_iface = false, is_enum = false, is_anno = false;
    std::string super_name, ifaces_str, annotations_str;

    if(g_jmid_classMods)   { cls_mods = env->CallIntMethod(klass, g_jmid_classMods);           raw_ExceptionClear(env); }
    if(g_jmid_isInterface) { is_iface = env->CallBooleanMethod(klass, g_jmid_isInterface) != 0; raw_ExceptionClear(env); }
    if(g_jmid_isEnum)      { is_enum  = env->CallBooleanMethod(klass, g_jmid_isEnum)      != 0; raw_ExceptionClear(env); }
    if(g_jmid_isAnnotation){ is_anno  = env->CallBooleanMethod(klass, g_jmid_isAnnotation) != 0;raw_ExceptionClear(env); }

    if(g_jmid_getSuperclass){
        jobject sc = raw_CallObjectMethod(env, klass, g_jmid_getSuperclass);
        raw_ExceptionClear(env);
        if(sc){ super_name = get_type_name(env, sc); raw_DeleteLocalRef(env, sc); }
    }
    if(g_jmid_getInterfaces){
        jobject iarr = raw_CallObjectMethod(env, klass, g_jmid_getInterfaces);
        raw_ExceptionClear(env);
        if(iarr){ ifaces_str = get_class_array_names(env, iarr); raw_DeleteLocalRef(env, iarr); }
    }
    if(g_jmid_getAnnotations){
        jobject aarr = raw_CallObjectMethod(env, klass, g_jmid_getAnnotations);
        raw_ExceptionClear(env);
        if(aarr){
            jint alen = env->GetArrayLength((jarray)aarr);
            for(jint i = 0; i < alen; i++){
                jobject aobj = env->GetObjectArrayElement((jobjectArray)aarr, i);
                raw_ExceptionClear(env);
                if(!aobj) continue;
                if(g_jmid_annoTypeName){
                    jobject atype = raw_CallObjectMethod(env, aobj, g_jmid_annoTypeName);
                    raw_ExceptionClear(env);
                    if(atype){
                        if(!annotations_str.empty()) annotations_str += ", ";
                        annotations_str += "@" + get_type_name(env, atype);
                        raw_DeleteLocalRef(env, atype);
                    }
                }
                raw_DeleteLocalRef(env, aobj);
            }
            raw_DeleteLocalRef(env, aarr);
        }
    }

    std::string kind;
    if(is_anno)            kind = "ANNOTATION";
    else if(is_iface)      kind = "INTERFACE";
    else if(is_enum)       kind = "ENUM";
    else if(cls_mods&0x400)kind = "ABSTRACT CLASS";
    else                   kind = "CLASS";

    bool is_final_cls = (cls_mods & 0x10) != 0;
    std::string access   = decode_access(cls_mods);
    std::string cls_tags = class_semantic_tags(simple_name, dotname, super_name, is_iface, is_enum);

    fprintf(g_fmt_file, "\n%s {\n", simple_name.c_str());
    fprintf(g_fmt_file, "  class:   %s\n", dotname.c_str());
    fprintf(g_fmt_file, "  kind:    %s %s%s%s\n", access.c_str(), kind.c_str(),
        is_final_cls ? " [final]" : "", cls_tags.c_str());
    if(!super_name.empty() && super_name != "java.lang.Object")
        fprintf(g_fmt_file, "  extends: %s\n", super_name.c_str());
    if(!ifaces_str.empty())
        fprintf(g_fmt_file, "  implements: [%s]\n", ifaces_str.c_str());
    if(!annotations_str.empty())
        fprintf(g_fmt_file, "  annotations: [%s]\n", annotations_str.c_str());
    fprintf(g_fmt_file, "\n");

    if(g_jmid_getDeclaredCtors && g_jcls_Constructor){
        jobject carr = raw_CallObjectMethod(env, klass, g_jmid_getDeclaredCtors);
        raw_ExceptionClear(env);
        if(carr){
            jint clen = env->GetArrayLength((jarray)carr);
            if(clen > 0){
                fprintf(g_fmt_file, "  constructors: {\n");
                for(jint i = 0; i < clen; i++){
                    jobject cobj = env->GetObjectArrayElement((jobjectArray)carr, i);
                    raw_ExceptionClear(env);
                    if(!cobj) continue;
                    std::string cparams;
                    if(g_jmid_ctorParams){
                        jobject parr = raw_CallObjectMethod(env, cobj, g_jmid_ctorParams);
                        raw_ExceptionClear(env);
                        if(parr){ cparams = get_class_array_names(env, parr); raw_DeleteLocalRef(env, parr); }
                    }
                    jint cmods = 0;
                    if(g_jmid_ctorMods){ cmods = env->CallIntMethod(cobj, g_jmid_ctorMods); raw_ExceptionClear(env); }
                    fprintf(g_fmt_file, "    %s(%s)  [%s]\n",
                        simple_name.c_str(), cparams.c_str(), decode_access(cmods).c_str());
                    raw_DeleteLocalRef(env, cobj);
                }
                fprintf(g_fmt_file, "  }\n\n");
            }
            raw_DeleteLocalRef(env, carr);
        }
        raw_ExceptionClear(env);
    }

    fprintf(g_fmt_file, "  fields: {\n");
    if(g_jmid_getDeclaredFields){
        jobject farr = raw_CallObjectMethod(env, klass, g_jmid_getDeclaredFields);
        raw_ExceptionClear(env);
        if(farr){
            jint flen = env->GetArrayLength((jarray)farr);
            for(jint i = 0; i < flen; i++){
                jobject fobj = env->GetObjectArrayElement((jobjectArray)farr, i);
                if(!fobj){ raw_ExceptionClear(env); continue; }
                std::string fname;
                if(g_jmid_fldName){
                    jstring js = (jstring)raw_CallObjectMethod(env, fobj, g_jmid_fldName);
                    raw_ExceptionClear(env);
                    if(js){ fname = jstr_to_std(env, js); raw_DeleteLocalRef(env, js); }
                }
                std::string ftype = "?";
                if(g_jmid_fldType){
                    jobject ft = raw_CallObjectMethod(env, fobj, g_jmid_fldType);
                    raw_ExceptionClear(env);
                    if(ft){ ftype = get_type_name(env, ft); raw_DeleteLocalRef(env, ft); }
                }
                jint mods = 0;
                if(g_jmid_fldMods){ mods = env->CallIntMethod(fobj, g_jmid_fldMods); raw_ExceptionClear(env); }
                bool is_static = (mods & 0x8) != 0;
                jlong offset = -1;
                if(g_unsafe){
                    jmethodID mid = is_static ? g_jmid_unsafeSFO : g_jmid_unsafeOFO;
                    if(mid){ offset = env->CallLongMethod(g_unsafe, mid, fobj); raw_ExceptionClear(env); }
                }
                std::string ftags   = field_semantic_tags(fname, ftype, mods);
                std::string faccess = decode_access(mods);
                if(!fname.empty()){
                    if(offset >= 0)
                        fprintf(g_fmt_file,
                            "    %-36s  %s  type: %-48s  offset: 0x%04llX (dec %-6lld)%s%s\n",
                            fname.c_str(), faccess.c_str(), ftype.c_str(),
                            (unsigned long long)offset, (long long)offset,
                            is_static ? " [static]" : "", ftags.c_str());
                    else
                        fprintf(g_fmt_file,
                            "    %-36s  %s  type: %s%s%s\n",
                            fname.c_str(), faccess.c_str(), ftype.c_str(),
                            is_static ? " [static]" : "", ftags.c_str());
                }
                raw_DeleteLocalRef(env, fobj);
            }
            raw_DeleteLocalRef(env, farr);
        }
        raw_ExceptionClear(env);
    }
    fprintf(g_fmt_file, "  }\n\n  methods: {\n");

    if(g_jmid_getDeclaredMethods){
        jobject marr = raw_CallObjectMethod(env, klass, g_jmid_getDeclaredMethods);
        raw_ExceptionClear(env);
        if(marr){
            jint mlen = env->GetArrayLength((jarray)marr);
            for(jint i = 0; i < mlen; i++){
                jobject mobj = env->GetObjectArrayElement((jobjectArray)marr, i);
                if(!mobj){ raw_ExceptionClear(env); continue; }
                std::string mname;
                if(g_jmid_mthName){
                    jstring js = (jstring)raw_CallObjectMethod(env, mobj, g_jmid_mthName);
                    raw_ExceptionClear(env);
                    if(js){ mname = jstr_to_std(env, js); raw_DeleteLocalRef(env, js); }
                }
                std::string ret_type = "void";
                if(g_jmid_mthRet){
                    jobject rt = raw_CallObjectMethod(env, mobj, g_jmid_mthRet);
                    raw_ExceptionClear(env);
                    if(rt){ ret_type = get_type_name(env, rt); raw_DeleteLocalRef(env, rt); }
                }
                std::string params_str;
                if(g_jmid_mthParams){
                    jobject parr = raw_CallObjectMethod(env, mobj, g_jmid_mthParams);
                    raw_ExceptionClear(env);
                    if(parr){ params_str = get_class_array_names(env, parr); raw_DeleteLocalRef(env, parr); }
                }
                std::string exc_str;
                if(g_jmid_mthExcTypes){
                    jobject earr = raw_CallObjectMethod(env, mobj, g_jmid_mthExcTypes);
                    raw_ExceptionClear(env);
                    if(earr){ exc_str = get_class_array_names(env, earr); raw_DeleteLocalRef(env, earr); }
                }
                jint mmods = 0;
                if(g_jmid_mthMods){ mmods = env->CallIntMethod(mobj, g_jmid_mthMods); raw_ExceptionClear(env); }
                bool m_static = (mmods & 0x8) != 0;
                std::string mtags   = method_semantic_tags(mname, ret_type, mmods);
                std::string maccess = decode_access(mmods);
                if(!mname.empty()){
                    fprintf(g_fmt_file, "    %-36s  %s(%s) -> %s%s%s\n",
                        mname.c_str(), maccess.c_str(), params_str.c_str(), ret_type.c_str(),
                        m_static ? " [static]" : "", mtags.c_str());
                    if(!exc_str.empty())
                        fprintf(g_fmt_file, "        throws: [%s]\n", exc_str.c_str());
                }
                raw_DeleteLocalRef(env, mobj);
            }
            raw_DeleteLocalRef(env, marr);
        }
        raw_ExceptionClear(env);
    }

    fprintf(g_fmt_file, "  }\n}\n");
    fflush(g_fmt_file);
}

static bool dump_class(jclass klass){
    if(!g_jmid_getName) return false;

    jstring jname = (jstring)raw_CallObjectMethod(g_env, klass, g_jmid_getName);
    if(!jname || raw_ExceptionCheck(g_env)){ raw_ExceptionClear(g_env); return false; }
    std::string dotname = jstr_to_std(g_env, jname);
    raw_DeleteLocalRef(g_env, jname);
    if(dotname.empty()) return false;
    
    if(dotname[0] == '[') return false;

    bool is_net_class = false;
    bool is_rise_crypto = false;
    {
        std::string lo = dotname;
        for(char& c : lo) c = (char)tolower((unsigned char)c);
        const char* nk[] = {"network","packet","handshake","netty","channel","socket","login","connect","netmanager","packethandler",nullptr};
        for(int ki = 0; nk[ki]; ki++) if(lo.find(nk[ki]) != std::string::npos){ is_net_class = true; break; }
        // Only flag craftrise.* or crsecond.* classes — not JDK's own crypto classes
        bool is_cr_pkg = (lo.find("craftrise.") == 0 || lo.find("crsecond.") == 0 ||
                          lo.find("craftrise/") == 0 || lo.find("crsecond/") == 0);
        if(is_cr_pkg){
            const char* ck[] = {"verify","encrypt","decrypt","cipher","crypto","secret","handshake","riseverify","risevoice","login","auth",nullptr};
            for(int ki = 0; ck[ki]; ki++) if(lo.find(ck[ki]) != std::string::npos){ is_rise_crypto = true; break; }
        }
    }

    std::string sig = "L";
    for(char c : dotname) sig += (c == '.' ? '/' : c);
    sig += ";";

    {std::lock_guard<std::mutex> lk(g_mutex); if(g_written.count(sig)) return false; g_written.insert(sig);}
    int idx = ++g_class_count;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        set_col(YELLOW); printf("[%5d] ", idx);
        set_col(CYAN);   printf("CLASS ");
        set_col(GREEN);  printf("%s\n", sig.c_str()); set_col(WHITE);
        if(g_file) fprintf(g_file, "CLASS %s\n", sig.c_str());
    }
    
    dump_class_fmt(g_env, klass, dotname);
    dump_class_schema(g_env, klass, dotname);
    if(is_net_class && g_net_file){
        fprintf(g_net_file, "=== %s ===\n", dotname.c_str());
        fflush(g_net_file);
    }

    bool jvmti_fields_ok = false;
    if(g_jvmti){
        jint fc = 0; jfieldID* fields = nullptr;
        jvmtiError fe = g_jvmti->GetClassFields(klass, &fc, &fields);
        if(fe == JVMTI_ERROR_NONE && fields && fc > 0){
            jvmti_fields_ok = true;
            for(jint i = 0; i < fc; i++){
                char *fn=nullptr, *fd=nullptr, *fg=nullptr;
                if(g_jvmti->GetFieldName(klass, fields[i], &fn, &fd, &fg) == JVMTI_ERROR_NONE && fn){
                    std::lock_guard<std::mutex> lk(g_mutex);
                    set_col(GREY);
                    printf("        FIELD  %-40s %s\n", fn, fd ? fd : "?");
                    set_col(WHITE);
                    if(g_file) fprintf(g_file, "  FIELD %s %s\n", fn, fd ? fd : "?");
                }
                if(fn) g_jvmti->Deallocate((unsigned char*)fn);
                if(fd) g_jvmti->Deallocate((unsigned char*)fd);
                if(fg) g_jvmti->Deallocate((unsigned char*)fg);
            }
            g_jvmti->Deallocate((unsigned char*)fields);
        } else if(fields){
            g_jvmti->Deallocate((unsigned char*)fields);
        }
    }
    
    if(!jvmti_fields_ok && g_jmid_getDeclaredFields && g_jmid_fldName){
        jobject farr = raw_CallObjectMethod(g_env, klass, g_jmid_getDeclaredFields);
        if(farr && !raw_ExceptionCheck(g_env)){
            jint flen = g_env->GetArrayLength((jarray)farr);
            for(jint i = 0; i < flen; i++){
                jobject fobj = g_env->GetObjectArrayElement((jobjectArray)farr, i);
                if(!fobj || raw_ExceptionCheck(g_env)){ raw_ExceptionClear(g_env); continue; }
                jstring fname = (jstring)raw_CallObjectMethod(g_env, fobj, g_jmid_fldName);
                std::string fn = jstr_to_std(g_env, fname);
                std::string ft;
                if(g_jmid_fldType){
                    raw_ExceptionClear(g_env);
                    jobject ftype = raw_CallObjectMethod(g_env, fobj, g_jmid_fldType);
                    if(ftype && !raw_ExceptionCheck(g_env)){
                        jstring ftname = (jstring)raw_CallObjectMethod(g_env, ftype, g_jmid_getName);
                        ft = jstr_to_std(g_env, ftname);
                        raw_ExceptionClear(g_env);
                        if(ftname) raw_DeleteLocalRef(g_env, ftname);
                    }
                    raw_ExceptionClear(g_env);
                    if(ftype) raw_DeleteLocalRef(g_env, ftype);
                }
                raw_ExceptionClear(g_env);
                if(fname) raw_DeleteLocalRef(g_env, fname);
                raw_DeleteLocalRef(g_env, fobj);
                if(!fn.empty()){
                    std::lock_guard<std::mutex> lk(g_mutex);
                    set_col(GREY);
                    printf("        FIELD  %-40s %s\n", fn.c_str(), ft.c_str());
                    set_col(WHITE);
                    if(g_file) fprintf(g_file, "  FIELD %s %s\n", fn.c_str(), ft.c_str());
                }
            }
        }
        raw_ExceptionClear(g_env);
        if(farr) raw_DeleteLocalRef(g_env, farr);
    }

    bool jvmti_methods_ok = false;
    if(g_jvmti){
        jint mc = 0; jmethodID* methods = nullptr;
        jvmtiError me = g_jvmti->GetClassMethods(klass, &mc, &methods);
        if(me == JVMTI_ERROR_NONE && methods && mc > 0){
            jvmti_methods_ok = true;
            for(jint i = 0; i < mc; i++){
                char *mn=nullptr, *md=nullptr, *mg=nullptr;
                if(g_jvmti->GetMethodName(methods[i], &mn, &md, &mg) == JVMTI_ERROR_NONE && mn){
                    std::lock_guard<std::mutex> lk(g_mutex);
                    set_col(GREY);
                    printf("        METHOD %-40s %s\n", mn, md ? md : "?");
                    set_col(WHITE);
                    if(g_file) fprintf(g_file, "  METHOD %s %s\n", mn, md ? md : "?");
                }
                if(mn) g_jvmti->Deallocate((unsigned char*)mn);
                if(md) g_jvmti->Deallocate((unsigned char*)md);
                if(mg) g_jvmti->Deallocate((unsigned char*)mg);
            }
            g_jvmti->Deallocate((unsigned char*)methods);
        } else if(methods){
            g_jvmti->Deallocate((unsigned char*)methods);
        }
    }
    
    if(!jvmti_methods_ok && g_jmid_getDeclaredMethods && g_jmid_mthName){
        jobject marr = raw_CallObjectMethod(g_env, klass, g_jmid_getDeclaredMethods);
        if(marr && !raw_ExceptionCheck(g_env)){
            jint mlen = g_env->GetArrayLength((jarray)marr);
            for(jint i = 0; i < mlen; i++){
                jobject mobj = g_env->GetObjectArrayElement((jobjectArray)marr, i);
                if(!mobj || raw_ExceptionCheck(g_env)){ raw_ExceptionClear(g_env); continue; }
                jstring mname = (jstring)raw_CallObjectMethod(g_env, mobj, g_jmid_mthName);
                std::string mn = jstr_to_std(g_env, mname);
                raw_ExceptionClear(g_env);
                if(mname) raw_DeleteLocalRef(g_env, mname);
                raw_DeleteLocalRef(g_env, mobj);
                if(!mn.empty()){
                    std::lock_guard<std::mutex> lk(g_mutex);
                    set_col(GREY);
                    printf("        METHOD %s\n", mn.c_str());
                    set_col(WHITE);
                    if(g_file) fprintf(g_file, "  METHOD %s\n", mn.c_str());
                }
            }
        }
        raw_ExceptionClear(g_env);
        if(marr) raw_DeleteLocalRef(g_env, marr);
    }

    if(is_net_class && g_net_file){
        if(g_jvmti){
            jint fc2 = 0; jfieldID* f2 = nullptr;
            if(g_jvmti->GetClassFields(klass, &fc2, &f2) == JVMTI_ERROR_NONE && f2){
                for(jint i = 0; i < fc2; i++){
                    char *fn=nullptr, *fd=nullptr, *fg=nullptr;
                    if(g_jvmti->GetFieldName(klass, f2[i], &fn, &fd, &fg) == JVMTI_ERROR_NONE && fn)
                        fprintf(g_net_file, "  FIELD  %-40s %s\n", fn, fd ? fd : "?");
                    if(fn) g_jvmti->Deallocate((unsigned char*)fn);
                    if(fd) g_jvmti->Deallocate((unsigned char*)fd);
                    if(fg) g_jvmti->Deallocate((unsigned char*)fg);
                }
                g_jvmti->Deallocate((unsigned char*)f2);
            }
            jint mc2 = 0; jmethodID* m2 = nullptr;
            if(g_jvmti->GetClassMethods(klass, &mc2, &m2) == JVMTI_ERROR_NONE && m2){
                for(jint i = 0; i < mc2; i++){
                    char *mn=nullptr, *md=nullptr, *mg=nullptr;
                    if(g_jvmti->GetMethodName(m2[i], &mn, &md, &mg) == JVMTI_ERROR_NONE && mn)
                        fprintf(g_net_file, "  METHOD %-40s %s\n", mn, md ? md : "?");
                    if(mn) g_jvmti->Deallocate((unsigned char*)mn);
                    if(md) g_jvmti->Deallocate((unsigned char*)md);
                    if(mg) g_jvmti->Deallocate((unsigned char*)mg);
                }
                g_jvmti->Deallocate((unsigned char*)m2);
            }
        }
        fprintf(g_net_file, "\n");
        fflush(g_net_file);
    }

    if(is_rise_crypto && g_crypto_file){
        std::vector<std::string> dummy_strings;
        // Write live JVMTI data for crypto class
        // First, try to get source file name for better labeling
        std::string label = dotname;
        if(g_jvmti){
            char* sf = nullptr;
            if(g_jvmti->GetSourceFileName(klass, &sf) == JVMTI_ERROR_NONE && sf){
                label += " (source: "; label += sf; label += ")";
                g_jvmti->Deallocate((unsigned char*)sf);
            }
        }
        write_crypto_entry(label, dummy_strings, klass);
    }

    {std::lock_guard<std::mutex> lk(g_mutex);
     printf("\n");
     if(g_file){ fprintf(g_file, "\n"); fflush(g_file); }
    }
    return true;
}

static void JNICALL class_file_load_hook(
    jvmtiEnv*, JNIEnv*, jclass, jobject, const char* name,
    jobject, jint class_data_len, const unsigned char* class_data,
    jint*, unsigned char**)
{
    if(!name) return;

    if(g_class_dump_mode.load() && class_data && class_data_len > 0 && g_class_dump_dir[0]){
        char rel[MAX_PATH] = {};
        int ri = 0;
        for(int ci = 0; name[ci] && ri < MAX_PATH - 2; ci++)
            rel[ri++] = (name[ci] == '/') ? '\\' : name[ci];
        char fpath[MAX_PATH];
        _snprintf_s(fpath, sizeof(fpath), "%s\\%s.class", g_class_dump_dir, rel);
        make_dirs_for(fpath);
        if(GetFileAttributesA(fpath) == INVALID_FILE_ATTRIBUTES){
            FILE* f = fopen(fpath, "wb");
            if(f){ fwrite(class_data, 1, (size_t)class_data_len, f); fclose(f); }
        }
        return;
    }

    std::string sig = "L"; sig += name; sig += ";";
    {std::lock_guard<std::mutex> lk(g_mutex); if(g_written.count(sig)) return; g_written.insert(sig);}
    int idx = ++g_class_count;
    std::lock_guard<std::mutex> lk(g_mutex);
    set_col(YELLOW); printf("[%5d] ", idx);
    set_col(CYAN); printf("HOOK  "); set_col(GREEN); printf("%s\n", sig.c_str()); set_col(WHITE);
    if(g_file){ fprintf(g_file, "CLASS %s\n\n", sig.c_str()); fflush(g_file); }
}


static const char* CRYPTO_KEYWORDS[] = {
    "AES","RSA","SHA","MD5","HMAC","Blowfish","DES","ChaCha","Cipher",
    "SecretKey","KeyPair","PublicKey","PrivateKey","KeyFactory","KeyGenerator",
    "SecureRandom","MessageDigest","Signature","javax/crypto","java/security/",
    "NoPadding","PKCS1","PKCS5","PKCS7","PKCS8","GCM","CFB","CBC","ECB","OFB","CTR",
    "TLS","SSL","X.509","Base64","BigInteger","encrypt","decrypt","hash","verify",
    "RiseVerify","RiseVoice","RiseKeepAlive","CraftRise","craftrise","crsecond",
    nullptr
};

static bool is_crypto_string(const char* s, size_t slen){
    for(int k = 0; CRYPTO_KEYWORDS[k]; k++){
        const char* kw = CRYPTO_KEYWORDS[k];
        size_t kwlen = strlen(kw);
        if(kwlen > slen) continue;
        for(size_t i = 0; i+kwlen <= slen; i++){
            if(memcmp(s+i, kw, kwlen)==0) return true;
        }
    }
    return false;
}

// Parse Java class file constant pool, collect UTF-8 strings matching crypto keywords.
// Returns list of matching strings.
static std::vector<std::string> parse_crypto_strings(const std::vector<uint8_t>& buf){
    std::vector<std::string> out;
    const uint8_t* d = buf.data();
    size_t len = buf.size();
    if(len < 10) return out;
    // magic check
    if(d[0]!=0xCA||d[1]!=0xFE||d[2]!=0xBA||d[3]!=0xBE) return out;
    size_t pos = 8; // skip magic(4)+minor(2)+major(2)
    if(pos+2>len) return out;
    int cp_count = ((int)d[pos]<<8)|d[pos+1]; pos+=2;
    for(int i=1;i<cp_count;i++){
        if(pos>=len) break;
        uint8_t tag = d[pos++];
        if(tag==1){ // CONSTANT_Utf8
            if(pos+2>len) break;
            int slen=((int)d[pos]<<8)|d[pos+1]; pos+=2;
            if(pos+(size_t)slen>len) break;
            if(slen>4 && is_crypto_string((char*)d+pos,(size_t)slen))
                out.push_back(std::string((char*)d+pos,(size_t)slen));
            pos+=slen;
        } else if(tag==3||tag==4) pos+=4;
        else if(tag==5||tag==6){pos+=8; i++;} // long/double take 2 slots
        else if(tag==7||tag==8||tag==16||tag==19||tag==20) pos+=2;
        else if(tag==9||tag==10||tag==11||tag==12||tag==17||tag==18) pos+=4;
        else if(tag==15) pos+=3;
        // unknown tag: bail out
        else break;
    }
    return out;
}

static void write_crypto_entry(const std::string& class_path,
                               const std::vector<std::string>& strings,
                               jclass klass){
    if(!g_crypto_file) return;
    fprintf(g_crypto_file, "================================================================\n");
    fprintf(g_crypto_file, "[CLASS] %s\n", class_path.c_str());

    // Print interesting constant pool strings
    if(!strings.empty()){
        fprintf(g_crypto_file, "  Constant pool references:\n");
        std::set<std::string> seen;
        for(auto& s : strings){
            if(seen.count(s) || s.size()<3) continue;
            seen.insert(s);
            fprintf(g_crypto_file, "    \"%s\"\n", s.c_str());
        }
    }

    if(klass && g_env){
        JNIEnv* env = g_env;
        // ── Fields via JNI reflection ──────────────────────────────────────
        fprintf(g_crypto_file, "  Fields:\n");
        if(g_jmid_getDeclaredFields){
            jobject farr = env->CallObjectMethod(klass, g_jmid_getDeclaredFields);
            env->ExceptionClear();
            if(farr){
                jint flen = env->GetArrayLength((jarray)farr);
                env->ExceptionClear();
                for(jint i=0;i<flen;i++){
                    jobject fobj = env->GetObjectArrayElement((jobjectArray)farr, i);
                    env->ExceptionClear();
                    if(!fobj){continue;}
                    // Field.getName()
                    jstring fname_js = nullptr;
                    if(g_jmid_getName) fname_js=(jstring)env->CallObjectMethod(fobj,g_jmid_getName);
                    env->ExceptionClear();
                    // Field.getType().getName()
                    jstring ftype_js = nullptr;
                    if(g_jmid_fldType){
                        jobject ftype = env->CallObjectMethod(fobj, g_jmid_fldType);
                        env->ExceptionClear();
                        if(ftype && g_jmid_getName)
                            ftype_js=(jstring)env->CallObjectMethod(ftype, g_jmid_getName);
                        env->ExceptionClear();
                        if(ftype) env->DeleteLocalRef(ftype);
                    }
                    std::string fn = fname_js ? jstr_to_std(env,fname_js) : "?";
                    std::string ft = ftype_js ? jstr_to_std(env,ftype_js) : "?";
                    fprintf(g_crypto_file,"    %-30s  %s\n", fn.c_str(), ft.c_str());
                    if(fname_js) env->DeleteLocalRef(fname_js);
                    if(ftype_js) env->DeleteLocalRef(ftype_js);
                    env->DeleteLocalRef(fobj);
                }
                env->DeleteLocalRef(farr);
            }
        }
        // ── Methods via JNI reflection ─────────────────────────────────────
        fprintf(g_crypto_file, "  Methods:\n");
        if(g_jmid_getDeclaredMethods){
            jobject marr = env->CallObjectMethod(klass, g_jmid_getDeclaredMethods);
            env->ExceptionClear();
            if(marr){
                jint mlen = env->GetArrayLength((jarray)marr);
                env->ExceptionClear();
                for(jint i=0;i<mlen;i++){
                    jobject mobj = env->GetObjectArrayElement((jobjectArray)marr, i);
                    env->ExceptionClear();
                    if(!mobj) continue;
                    jstring mname_js = nullptr;
                    if(g_jmid_getName) mname_js=(jstring)env->CallObjectMethod(mobj,g_jmid_getName);
                    env->ExceptionClear();
                    jstring rtype_js = nullptr;
                    if(g_jmid_mthRet){
                        jobject rt = env->CallObjectMethod(mobj, g_jmid_mthRet);
                        env->ExceptionClear();
                        if(rt && g_jmid_getName) rtype_js=(jstring)env->CallObjectMethod(rt,g_jmid_getName);
                        env->ExceptionClear();
                        if(rt) env->DeleteLocalRef(rt);
                    }
                    // Parameter types
                    std::string params;
                    if(g_jmid_mthParams){
                        jobject ptarr = env->CallObjectMethod(mobj, g_jmid_mthParams);
                        env->ExceptionClear();
                        if(ptarr){
                            jint plen = env->GetArrayLength((jarray)ptarr);
                            env->ExceptionClear();
                            for(jint j=0;j<plen;j++){
                                jobject pt = env->GetObjectArrayElement((jobjectArray)ptarr,j);
                                env->ExceptionClear();
                                if(pt){
                                    if(!params.empty()) params+=", ";
                                    if(g_jmid_getName){
                                        jstring pn=(jstring)env->CallObjectMethod(pt,g_jmid_getName);
                                        env->ExceptionClear();
                                        if(pn){ params+=jstr_to_std(env,pn); env->DeleteLocalRef(pn); }
                                    }
                                    env->DeleteLocalRef(pt);
                                }
                            }
                            env->DeleteLocalRef(ptarr);
                        }
                    }
                    std::string mn = mname_js ? jstr_to_std(env,mname_js) : "?";
                    std::string rt = rtype_js ? jstr_to_std(env,rtype_js) : "?";
                    fprintf(g_crypto_file,"    %s(%s) -> %s\n", mn.c_str(), params.c_str(), rt.c_str());
                    if(mname_js) env->DeleteLocalRef(mname_js);
                    if(rtype_js) env->DeleteLocalRef(rtype_js);
                    env->DeleteLocalRef(mobj);
                }
                env->DeleteLocalRef(marr);
            }
        }
    }
    fprintf(g_crypto_file, "\n");
    fflush(g_crypto_file);
}


static void extract_jars_from_classpath(JNIEnv* /*hint_env*/){
    if(g_class_dump_dir[0] == '\0' || !g_jvm) return;

    // Always attach current thread — using another thread's JNIEnv for NewObject/NewArray crashes JVM
    JNIEnv* env = nullptr;
    JavaVMAttachArgs att = {JNI_VERSION_1_8, (char*)"CRDumper-Extract", nullptr};
    bool we_attached = false;
    jint att_rc = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8);
    if(att_rc == JNI_EDETACHED){
        if(g_jvm->AttachCurrentThread((void**)&env, &att) == JNI_OK) we_attached = true;
    }
    if(!env){ con_log(RED, "[!] extract: JNIEnv alinamadi\n"); return; }

    jclass sys_cls = env->FindClass("java/lang/System");
    env->ExceptionClear();
    if(!sys_cls){ con_log(RED, "[!] System class bulunamadi\n"); return; }
    jmethodID get_prop = env->GetStaticMethodID(sys_cls, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
    env->ExceptionClear();
    if(!get_prop){ env->DeleteLocalRef(sys_cls); return; }
    jstring key = env->NewStringUTF("java.class.path");
    jstring cp_str = (jstring)env->CallStaticObjectMethod(sys_cls, get_prop, key);
    env->ExceptionClear();
    env->DeleteLocalRef(key); env->DeleteLocalRef(sys_cls);
    if(!cp_str) return;
    std::string classpath = jstr_to_std(env, cp_str);
    env->DeleteLocalRef(cp_str);

    std::vector<std::string> jars;
    size_t s = 0;
    for(size_t i = 0; i <= classpath.size(); i++){
        if(i == classpath.size() || classpath[i] == ';'){
            std::string part = classpath.substr(s, i - s);
            if(part.size() > 4){
                std::string ext = part.substr(part.size()-4);
                for(char& c : ext) c = (char)tolower((unsigned char)c);
                if(ext == ".jar" || ext == ".zip") jars.push_back(part);
            }
            s = i + 1;
        }
    }
    con_log(WHITE, "[*] Classpath: %zu JAR bulundu\n", jars.size());

    jclass zip_cls = env->FindClass("java/util/zip/ZipFile");
    env->ExceptionClear();
    if(!zip_cls){ con_log(RED, "[!] ZipFile class yok\n"); return; }

    jmethodID zip_ctor    = env->GetMethodID(zip_cls, "<init>",        "(Ljava/lang/String;)V"); env->ExceptionClear();
    jmethodID zip_entries = env->GetMethodID(zip_cls, "entries",       "()Ljava/util/Enumeration;"); env->ExceptionClear();
    jmethodID zip_stream  = env->GetMethodID(zip_cls, "getInputStream","(Ljava/util/zip/ZipEntry;)Ljava/io/InputStream;"); env->ExceptionClear();
    jmethodID zip_close   = env->GetMethodID(zip_cls, "close",         "()V"); env->ExceptionClear();
    jmethodID zip_size    = env->GetMethodID(zip_cls, "size",          "()I"); env->ExceptionClear();

    jclass enum_cls = env->FindClass("java/util/Enumeration"); env->ExceptionClear();
    jmethodID enum_has  = enum_cls ? (env->GetMethodID(enum_cls, "hasMoreElements", "()Z"),           env->ExceptionClear(), env->GetMethodID(enum_cls, "hasMoreElements", "()Z"))           : nullptr;
    jmethodID enum_next = enum_cls ? (env->GetMethodID(enum_cls, "nextElement",     "()Ljava/lang/Object;"), env->ExceptionClear(), env->GetMethodID(enum_cls, "nextElement", "()Ljava/lang/Object;")) : nullptr;
    env->ExceptionClear();
    if(enum_cls) env->DeleteLocalRef(enum_cls);

    jclass entry_cls = env->FindClass("java/util/zip/ZipEntry"); env->ExceptionClear();
    jmethodID entry_name = entry_cls ? env->GetMethodID(entry_cls, "getName", "()Ljava/lang/String;") : nullptr; env->ExceptionClear();
    if(entry_cls) env->DeleteLocalRef(entry_cls);

    jclass is_cls = env->FindClass("java/io/InputStream"); env->ExceptionClear();
    jmethodID is_read  = is_cls ? env->GetMethodID(is_cls, "read",  "([B)I")  : nullptr; env->ExceptionClear();
    jmethodID is_close = is_cls ? env->GetMethodID(is_cls, "close", "()V")    : nullptr; env->ExceptionClear();
    if(is_cls) env->DeleteLocalRef(is_cls);

    con_log(WHITE, "[*] ZipFile metodlari: ctor=%p entries=%p stream=%p enum_has=%p enum_next=%p entry_name=%p is_read=%p\n",
        (void*)zip_ctor,(void*)zip_entries,(void*)zip_stream,(void*)enum_has,(void*)enum_next,(void*)entry_name,(void*)is_read);

    if(!zip_ctor || !zip_entries || !zip_stream || !enum_has || !enum_next || !entry_name || !is_read){
        con_log(RED, "[!] ZipFile metot ID'leri eksik — JAR copy moduna gec\n");
        env->DeleteLocalRef(zip_cls);
        zip_cls = nullptr;
    }

    jbyteArray jbuf = zip_cls ? env->NewByteArray(65536) : nullptr;
    env->ExceptionClear();
    jint total = 0;

    for(const std::string& jpath : jars){
        const char* bname = jpath.c_str();
        for(const char* p = bname; *p; p++) if(*p == '\\' || *p == '/') bname = p+1;

        if(!zip_cls){ continue; }
        jstring jp = env->NewStringUTF(jpath.c_str());
        if(!jp){ env->ExceptionClear(); continue; }
        jobject zf = env->NewObject(zip_cls, zip_ctor, jp);
        if(env->ExceptionCheck()){
            jthrowable ex = env->ExceptionOccurred(); env->ExceptionClear();
            if(ex){
                jclass ex_cls = env->GetObjectClass(ex);
                jmethodID to_str = ex_cls ? env->GetMethodID(ex_cls, "toString", "()Ljava/lang/String;") : nullptr;
                env->ExceptionClear();
                if(to_str){ jstring jmsg = (jstring)env->CallObjectMethod(ex, to_str); env->ExceptionClear();
                    if(jmsg){ std::string msg = jstr_to_std(env, jmsg); env->DeleteLocalRef(jmsg);
                        con_log(YELLOW, "[~] ZipFile(%s): %s\n", bname, msg.c_str()); }
                }
                if(ex_cls) env->DeleteLocalRef(ex_cls);
                env->DeleteLocalRef(ex);
            }
        }
        env->DeleteLocalRef(jp);
        if(!zf) continue;

        jobject enm = env->CallObjectMethod(zf, zip_entries);
        env->ExceptionClear();

        while(enm && env->CallBooleanMethod(enm, enum_has)){
            env->ExceptionClear();
            jobject entry = env->CallObjectMethod(enm, enum_next);
            env->ExceptionClear();
            if(!entry) continue;

            std::string ename;
            if(entry_name){
                jstring jn = (jstring)env->CallObjectMethod(entry, entry_name);
                env->ExceptionClear();
                if(jn){ ename = jstr_to_std(env, jn); env->DeleteLocalRef(jn); }
            }

            if(ename.size() > 6 && ename.substr(ename.size()-6) == ".class"){
                std::string rel = ename;
                for(char& c : rel) if(c == '/') c = '\\';
                char fpath[MAX_PATH];
                _snprintf_s(fpath, sizeof(fpath), "%s\\%s", g_class_dump_dir, rel.c_str());

                if(!wfile_exists(fpath)){
                    jobject is = env->CallObjectMethod(zf, zip_stream, entry);
                    env->ExceptionClear();
                    if(is){
                        FILE* out = wfopen_write(fpath);
                        jint nr;
                        while((nr = env->CallIntMethod(is, is_read, jbuf)) > 0){
                            env->ExceptionClear();
                            jbyte* b = env->GetByteArrayElements(jbuf, nullptr);
                            if(b){ if(out) fwrite(b, 1, (size_t)nr, out); env->ReleaseByteArrayElements(jbuf, b, JNI_ABORT); }
                        }
                        env->ExceptionClear();
                        if(out){ fclose(out); total++; }
                        if(is_close){ env->CallVoidMethod(is, is_close); env->ExceptionClear(); }
                        env->DeleteLocalRef(is);
                    }
                }
            }
            env->DeleteLocalRef(entry);
        }
        if(enm) env->DeleteLocalRef(enm);
        if(zip_close){ env->CallVoidMethod(zf, zip_close); env->ExceptionClear(); }
        env->DeleteLocalRef(zf);
    }

    if(jbuf) env->DeleteLocalRef(jbuf);
    env->DeleteLocalRef(zip_cls);
    con_log(GREEN, "[+] JAR dump tamam: %d .class dosyasi yazildi\n\n", (int)total);
    if(we_attached) g_jvm->DetachCurrentThread();
}


// Walk extracted .class files in craftrisedumped/, scan constant pool for crypto refs
static void scan_classfiles_for_crypto(){
    if(!g_crypto_file || g_class_dump_dir[0]=='\0') return;
    // We only want craftrise/ and crsecond/ subdirs
    const char* pkg_roots[] = {"craftrise", "crsecond", nullptr};
    // Use Windows FindFirstFile recursion
    struct ScanCtx {
        static void walk(const std::string& dir, const char* pkg_root, FILE* out, JNIEnv* env){
            std::string pat = dir + "\\*";
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(pat.c_str(), &fd);
            if(h==INVALID_HANDLE_VALUE) return;
            do {
                if(fd.cFileName[0]=='.') continue;
                std::string full = dir + "\\" + fd.cFileName;
                if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
                    walk(full, pkg_root, out, env);
                } else {
                    size_t nlen = strlen(fd.cFileName);
                    if(nlen>6 && strcmp(fd.cFileName+nlen-6,".class")==0){
                        // Read file
                        FILE* f = fopen(full.c_str(),"rb");
                        if(!f) continue;
                        fseek(f,0,SEEK_END); long fsz=ftell(f); fseek(f,0,SEEK_SET);
                        if(fsz<10||fsz>2*1024*1024){ fclose(f); continue; }
                        std::vector<uint8_t> buf((size_t)fsz);
                        fread(buf.data(),1,(size_t)fsz,f); fclose(f);
                        // Check magic
                        if(buf[0]!=0xCA||buf[1]!=0xFE||buf[2]!=0xBA||buf[3]!=0xBE) continue;
                        // Parse constant pool for crypto refs
                        auto strs = parse_crypto_strings(buf);
                        // Filter to real crypto (not just "craftrise")
                        std::vector<std::string> hits;
                        const char* cfilter[] = {
                            "AES","RSA","SHA","Cipher","SecretKey","KeyFactory","KeyGenerator",
                            "MessageDigest","SecureRandom","NoPadding","PKCS","GCM","CFB8","CBC",
                            "javax/crypto","java/security/MessageDigest","java/security/KeyPair",
                            "RiseVerify","RiseVoice","RiseKeepAlive","verify","encrypt","decrypt",
                            "handshake","login","auth","token","hmac","HMAC",nullptr};
                        for(auto& s : strs){
                            bool ok=false;
                            for(int k=0;cfilter[k];k++) if(s.find(cfilter[k])!=std::string::npos){ok=true;break;}
                            if(ok) hits.push_back(s);
                        }
                        if(hits.empty()) continue;
                        // Derive class dotname from path
                        std::string class_name = full;
                        // Remove g_class_dump_dir prefix
                        // Replace \ with .
                        std::string prefix = g_class_dump_dir;
                        if(class_name.substr(0,prefix.size())==prefix)
                            class_name = class_name.substr(prefix.size()+1);
                        // Remove .class suffix
                        if(class_name.size()>6) class_name=class_name.substr(0,class_name.size()-6);
                        for(char& c:class_name) if(c=='\\') c='.';
                        // Try to find loaded jclass
                        std::string slashname = class_name;
                        for(char& c:slashname) if(c=='.') c='/';
                        jclass kls = nullptr;
                        if(env){
                            kls = env->FindClass(slashname.c_str());
                            env->ExceptionClear();
                        }
                        write_crypto_entry(class_name, hits, kls);
                        if(kls && env) env->DeleteLocalRef(kls);
                    }
                }
            } while(FindNextFileA(h,&fd));
            FindClose(h);
        }
    };
    // Attach current thread
    JNIEnv* env=nullptr; bool att=false;
    JavaVMAttachArgs a={JNI_VERSION_1_8,(char*)"CRDumper-Crypto",nullptr};
    if(g_jvm){ jint r=g_jvm->GetEnv((void**)&env,JNI_VERSION_1_8); if(r==JNI_EDETACHED&&g_jvm->AttachCurrentThread((void**)&env,&a)==JNI_OK) att=true; }
    fprintf(g_crypto_file,"================================================================\n");
    fprintf(g_crypto_file,"  Craftrise/crsecond classes referencing crypto APIs:\n");
    fprintf(g_crypto_file,"================================================================\n\n");
    for(int pi=0; pkg_roots[pi]; pi++){
        std::string pkg_dir = std::string(g_class_dump_dir) + "\\" + pkg_roots[pi];
        ScanCtx::walk(pkg_dir, pkg_roots[pi], g_crypto_file, env);
    }
    if(att && g_jvm) g_jvm->DetachCurrentThread();
    con_log(GREEN,"[+] Crypto scan tamam\n");
}


static DWORD WINAPI class_monitor_thread(LPVOID){
    while(!g_dump_done.load()) Sleep(1000);
    Sleep(5000);
    int round = 1;
    while(true){
        Sleep(10000);
        if(!g_jvm || !g_jvmti) continue;
        JNIEnv* env2 = nullptr;
        JavaVMAttachArgs args = {JNI_VERSION_1_8, (char*)"CRDumper-Monitor", nullptr};
        if(g_jvm->AttachCurrentThread((void**)&env2, &args) != JNI_OK || !env2) continue;

        jint cnt = 0; jclass* cls = nullptr;
        if(g_jvmti->GetLoadedClasses(&cnt, &cls) != JVMTI_ERROR_NONE || !cls){
            g_jvm->DetachCurrentThread(); continue;
        }

        jint new_count = 0;
        for(jint i = 0; i < cnt; i++){
            char* sig_buf = nullptr;
            if(g_jvmti->GetClassSignature(cls[i], &sig_buf, nullptr) != JVMTI_ERROR_NONE) continue;
            std::string sig(sig_buf); g_jvmti->Deallocate((unsigned char*)sig_buf);
            if(sig.empty() || sig[0] != 'L') continue;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                if(g_written.count(sig)) continue;
            }
            dump_class(cls[i]);
            new_count++;
        }
        g_jvmti->Deallocate((unsigned char*)cls);
        g_jvm->DetachCurrentThread();

        if(new_count > 0){
            con_log(GREEN, "[+] Monitor tur %d: %d yeni class bulundu\\n", round, (int)new_count);
            if(g_file) fflush(g_file);
            if(g_fmt_file) fflush(g_fmt_file);
            if(g_net_file) fflush(g_net_file);
        }
        round++;
    }
    return 0;
}


// Force-load all classes from classpath JARs so they appear in next snapshot
// This catches craftrise/crsecond lazy-loaded classes
static void force_load_all_jar_classes(JNIEnv* env){
    if(!env || !g_jvmti) return;
    con_log(CYAN, "[*] JAR sinif force-load baslatiliyor...\n");
    int loaded = 0, failed = 0;

    // Reuse classpath scanning logic
    jclass sys_cls = env->FindClass("java/lang/System"); env->ExceptionClear();
    if(!sys_cls) return;
    jmethodID get_prop = env->GetStaticMethodID(sys_cls, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;"); env->ExceptionClear();
    if(!get_prop){ env->DeleteLocalRef(sys_cls); return; }
    jstring key = env->NewStringUTF("java.class.path"); env->ExceptionClear();
    jstring cp_str = (jstring)env->CallStaticObjectMethod(sys_cls, get_prop, key); env->ExceptionClear();
    env->DeleteLocalRef(key); env->DeleteLocalRef(sys_cls);
    if(!cp_str) return;
    std::string classpath = jstr_to_std(env, cp_str); env->DeleteLocalRef(cp_str);

    // Get ZipFile class and methods
    jclass zip_cls = env->FindClass("java/util/zip/ZipFile"); env->ExceptionClear();
    jmethodID zip_ctor=nullptr, zip_entries=nullptr, enum_has=nullptr, enum_next=nullptr, entry_name=nullptr, zip_close=nullptr;
    if(zip_cls){
        zip_ctor   = env->GetMethodID(zip_cls,"<init>","(Ljava/lang/String;)V"); env->ExceptionClear();
        zip_entries= env->GetMethodID(zip_cls,"entries","()Ljava/util/Enumeration;"); env->ExceptionClear();
        zip_close  = env->GetMethodID(zip_cls,"close","()V"); env->ExceptionClear();
    }
    jclass enum_cls = env->FindClass("java/util/Enumeration"); env->ExceptionClear();
    if(enum_cls){
        enum_has  = env->GetMethodID(enum_cls,"hasMoreElements","()Z"); env->ExceptionClear();
        enum_next = env->GetMethodID(enum_cls,"nextElement","()Ljava/lang/Object;"); env->ExceptionClear();
        env->DeleteLocalRef(enum_cls);
    }
    jclass ze_cls = env->FindClass("java/util/zip/ZipEntry"); env->ExceptionClear();
    if(ze_cls){
        entry_name = env->GetMethodID(ze_cls,"getName","()Ljava/lang/String;"); env->ExceptionClear();
        env->DeleteLocalRef(ze_cls);
    }
    if(!zip_cls || !zip_ctor || !zip_entries || !enum_has || !enum_next || !entry_name){
        if(zip_cls) env->DeleteLocalRef(zip_cls);
        return;
    }

    // Parse classpath
    std::vector<std::string> jars;
    size_t s2=0;
    for(size_t i=0;i<=classpath.size();i++){
        if(i==classpath.size()||classpath[i]==';'){
            std::string part=classpath.substr(s2,i-s2);
            if(part.size()>4){
                std::string lo=part; for(char& c:lo) c=(char)tolower((unsigned char)c);
                if(lo.substr(lo.size()-4)==".jar") jars.push_back(part);
            }
            s2=i+1;
        }
    }

    for(const std::string& jpath : jars){
        jstring jp = env->NewStringUTF(jpath.c_str()); env->ExceptionClear();
        if(!jp) continue;
        jobject zf = env->NewObject(zip_cls, zip_ctor, jp); env->ExceptionClear();
        env->DeleteLocalRef(jp);
        if(!zf) continue;
        jobject enm = env->CallObjectMethod(zf, zip_entries); env->ExceptionClear();
        while(enm && env->CallBooleanMethod(enm, enum_has)){
            env->ExceptionClear();
            jobject entry = env->CallObjectMethod(enm, enum_next); env->ExceptionClear();
            if(!entry) break;
            jstring jn = (jstring)env->CallObjectMethod(entry, entry_name); env->ExceptionClear();
            if(jn){
                std::string ename = jstr_to_std(env, jn); env->DeleteLocalRef(jn);
                if(ename.size()>6 && ename.substr(ename.size()-6)==".class"){
                    std::string slash_name = ename.substr(0, ename.size()-6);
                    // FindClass forces JVM to load it
                    jclass kls = env->FindClass(slash_name.c_str());
                    env->ExceptionClear();
                    if(kls){ loaded++; env->DeleteLocalRef(kls); }
                    else failed++;
                    if((loaded+failed) % 500 == 0) Sleep(5); // yield
                }
            }
            env->DeleteLocalRef(entry);
        }
        if(enm) env->DeleteLocalRef(enm);
        if(zip_close){ env->CallVoidMethod(zf, zip_close); env->ExceptionClear(); }
        env->DeleteLocalRef(zf);
    }
    if(zip_cls) env->DeleteLocalRef(zip_cls);
    con_log(CYAN, "[+] Force-load: %d yuklendi, %d yuklenemedi\n", loaded, failed);
}


static void do_snapshot(int round = 0){
    if(!g_jvmti) return;
    jint cnt = 0; jclass* cls = nullptr;
    jvmtiError err = g_jvmti->GetLoadedClasses(&cnt, &cls);
    if(err != JVMTI_ERROR_NONE || !cls){
        con_log(RED, "[!] GetLoadedClasses err=%d\n", (int)err); return;
    }
    con_log(WHITE, "[*] Snapshot round=%d: %d class\n", round, cnt);
    int new_cnt = 0;
    for(jint i = 0; i < cnt; i++){
        if(dump_class(cls[i])) new_cnt++;
        if(i % 50 == 0 && i > 0) Sleep(2); // yield — prevent game freeze
    }
    g_jvmti->Deallocate((unsigned char*)cls);
    con_log(GREEN, "[+] Round %d tamam: %d yeni class\n", round, new_cnt);
    if(g_file) fflush(g_file);
    if(g_fmt_file) fflush(g_fmt_file);
    if(g_schema_file) fflush(g_schema_file);
}
static void initialize_and_dump(JNIEnv* env){
    g_env = env;
    if(env->GetJavaVM(&g_jvm) != JNI_OK || !g_jvm){
        con_log(RED, "[!] GetJavaVM basarisiz\n"); return;
    }
    con_log(GREEN, "[+] JavaVM*: %p\n", g_jvm);

    jvmtiEnv* jvmti = nullptr;
    if(g_jvm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_2) != JNI_OK || !jvmti){
        con_log(RED, "[!] jvmtiEnv* alinamadi\n"); return;
    }
    g_jvmti = jvmti;
    con_log(GREEN, "[+] jvmtiEnv*: %p\n", jvmti);

    void** fn_tbl = *(void***)jvmti;
    con_log(WHITE, "[*] jvmtiEnv funcs@: %p in_jvm=%d\n", (void*)fn_tbl, is_in_jvm((uintptr_t)fn_tbl));

    init_jni_pointers(env);
    if(init_jni_reflection(env)){
        con_log(GREEN, "[+] JNI reflection hazir\n");
    } else {
        con_log(YELLOW, "[!] JNI reflection kismi basarisiz\n");
    }

    jvmtiCapabilities caps = {};
    caps.can_generate_all_class_hook_events = 1;
    caps.can_retransform_classes            = 1;
    caps.can_retransform_any_class          = 1;
    caps.can_get_bytecodes                  = 1;
    jvmtiError cap_err = jvmti->AddCapabilities(&caps);
    con_log(WHITE, "[*] AddCapabilities: %d\n", (int)cap_err);


    {
        char map_path[MAX_PATH] = {};
        _snprintf_s(map_path, sizeof(map_path), "%s\\mappings.txt", g_outdir);
        fopen_s(&g_file, map_path, "w");
        if(g_file) con_log(GREEN, "[+] Dosya acildi: %s\n", map_path);
        else        con_log(YELLOW, "[!] mappings.txt acilamadi\n");
    }
    if(g_file){
        fprintf(g_file,
            "# CRNativeDumper -- By cmmdx256\n"
            "# CLASS L<internal/Name>;\n"
            "#   FIELD  <name> <type>\n"
            "#   METHOD <name> <descriptor>\n\n");
        fflush(g_file);
    }

    {
        char fmt_path[MAX_PATH] = {};
        _snprintf_s(fmt_path, sizeof(fmt_path), "%s\\mappings_fmt.txt", g_outdir);
        g_fmt_file = _fsopen(fmt_path, "w", _SH_DENYNO);
        if(g_fmt_file) con_log(GREEN, "[+] Fmt dosya: %s\n", fmt_path);
        else            con_log(YELLOW, "[!] mappings_fmt.txt acilamadi\n");
    }
    {
        char net_path[MAX_PATH] = {};
        _snprintf_s(net_path, sizeof(net_path), "%s\\network_handshake.txt", g_outdir);
        g_net_file = _fsopen(net_path, "w", _SH_DENYNO);
        if(g_net_file) con_log(GREEN, "[+] Net dosya: %s\n", net_path);
    }
    {
        char sch_path[MAX_PATH] = {};
        _snprintf_s(sch_path, sizeof(sch_path), "%s\\mappings_schema.txt", g_outdir);
        g_schema_file = _fsopen(sch_path, "w", _SH_DENYNO);
        if(g_schema_file){
            fprintf(g_schema_file, "CRNativeDumper -- Class Schema Map --By cmmdx256\n\n");
            con_log(GREEN, "[+] Schema dosya: %s\n", sch_path);
        }
    }
    {
        char cry_path[MAX_PATH] = {};
        _snprintf_s(cry_path, sizeof(cry_path), "%s\\handshake_crypto.txt", g_outdir);
        g_crypto_file = _fsopen(cry_path, "w", _SH_DENYNO);
        if(g_crypto_file){
            fprintf(g_crypto_file, "CRNativeDumper -- Handshake & Crypto Analysis --By cmmdx256\n");
            fprintf(g_crypto_file, "================================================================\n\n");
            con_log(GREEN, "[+] Crypto dosya: %s\n", cry_path);
        }
    }
    printf("\n");

    if(cap_err == JVMTI_ERROR_NONE){
        jvmtiEventCallbacks cbs = {}; cbs.ClassFileLoadHook = class_file_load_hook;
        jvmti->SetEventCallbacks(&cbs, (jint)sizeof(cbs));
        jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr);
        con_log(WHITE, "[*] ClassFileLoadHook aktif\n\n");
    }

    // Round 0: initial dump — classes loaded at startup
    do_snapshot(0);

    // JAR extraction in parallel (non-blocking)
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        extract_jars_from_classpath(g_env);
        scan_classfiles_for_crypto();
        return 0;
    }, nullptr, 0, nullptr);

    // Multi-round snapshot thread: catches lazy-loaded + server-join classes
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        JavaVMAttachArgs att = {JNI_VERSION_1_8, (char*)"CRDumper-Rounds", nullptr};
        JNIEnv* renv = nullptr; bool att2 = false;
        if(g_jvm->GetEnv((void**)&renv, JNI_VERSION_1_8) == JNI_EDETACHED)
            if(g_jvm->AttachCurrentThread((void**)&renv, &att) == JNI_OK) att2 = true;
        if(!renv){ if(att2) g_jvm->DetachCurrentThread(); return 0; }

        // Round 1: 45s — after main menu loads
        Sleep(45000);
        con_log(CYAN, "[*] Round 1 baslatiliyor (45s)...\n");
        do_snapshot(1);

        // Round 2: force-load + 120s — after server join
        Sleep(75000); // total 120s
        con_log(CYAN, "[*] Round 2 — force-load + snapshot (120s)...\n");
        force_load_all_jar_classes(renv);
        Sleep(3000);
        do_snapshot(2);

        // Round 3: 300s — all lazy initialized classes
        Sleep(180000); // total 300s
        con_log(CYAN, "[*] Round 3 — final snapshot (300s)...\n");
        do_snapshot(3);
        if(g_file)    { fprintf(g_file,     "\n[~] Tum roundlar tamamlandi\n"); fflush(g_file); }

        if(att2) g_jvm->DetachCurrentThread();
        return 0;
    }, nullptr, 0, nullptr);

    CreateThread(nullptr, 0, class_monitor_thread, nullptr, 0, nullptr);

    con_log(GREEN, "\n--By cmmdx256\n\n");
    if(g_file)    { fprintf(g_file,     "\n--By cmmdx256\n"); fflush(g_file); }
    if(g_fmt_file){ fprintf(g_fmt_file, "\n--By cmmdx256\n"); fflush(g_fmt_file); }
    if(g_dbg)     { fprintf(g_dbg,      "\n--By cmmdx256\n"); fflush(g_dbg); }
    if(g_net_file){ fprintf(g_net_file, "\n--By cmmdx256\n"); fflush(g_net_file); }
    if(g_schema_file){ fprintf(g_schema_file, "\n--By cmmdx256\n"); fflush(g_schema_file); }
    if(g_crypto_file){ fprintf(g_crypto_file, "\n-- END --By cmmdx256\n"); fflush(g_crypto_file); }

    g_dump_done.store(true);
    con_log(WHITE, "[*] Dump tamamlandi. Hook canli.\n");
}

static HMODULE g_hJvm = nullptr;
static std::atomic<bool> g_init_started{false};

static void __stdcall hooked_wglSwapBuffers(HDC hdc){
    g_gl_ready.store(true);

    if(!g_env_ready.load() && g_hJvm && !g_init_started.exchange(true)){
        JNIEnv* env = nullptr;
        __try{
            auto fn = (tInternalGetEnv)((uintptr_t)g_hJvm + JDK8U51_GET_ENV_OFFSET);
            jint rc = fn(nullptr, (void**)&env, JNI_VERSION_1_8);
            con_log(WHITE, "[*] JDK8u51 getter rc=%d env=%p\n", rc, env);
        }__except(EXCEPTION_EXECUTE_HANDLER){ env = nullptr; }

        if(env){
            con_log(GREEN, "[+] JNIEnv* render thread'den: %p\n", env);
            g_env_ready.store(true);
            __try{
                initialize_and_dump(env);
            }__except(EXCEPTION_EXECUTE_HANDLER){
                con_log(RED, "[!] initialize_and_dump SEH exception: 0x%08lX\n",
                    GetExceptionCode());
                if(g_dbg) fflush(g_dbg);
            }
        } else {
            con_log(YELLOW, "[~] JDK8u51 getter miss — worker thread deniyor\n");
            g_init_started.store(false);
        }
    }

    orig_wglSwapBuffers(hdc);
}

struct TBI{LONG ExitStatus;DWORD _pad;PVOID TebBaseAddress;PVOID Pid;PVOID Tid;ULONG_PTR Affinity;LONG Pri;LONG BasePri;};

static JNIEnv* scan_thread_for_jnienv(uintptr_t tls_val){
    if(tls_val<0x10000||tls_val>0x7FFFFFFFFFFFFull) return nullptr;
    __try{
        uintptr_t vtable=*(uintptr_t*)tls_val;
        if(!is_in_jvm(vtable)) return nullptr;
        uintptr_t fn0=*(uintptr_t*)vtable;
        if(!is_in_jvm(fn0)) return nullptr;
    }__except(1){return nullptr;}
    uint8_t* base=(uint8_t*)tls_val;
    for(int off=0;off<16384;off+=8){
        __try{
            uintptr_t cand=*(uintptr_t*)(base+off);
            if(looks_like_jni_functions(cand))
                return (JNIEnv*)(base+off);
        }__except(1){}
    }
    return nullptr;
}

static JNIEnv* teb_scan_all_threads(){
    if(!g_NtQIT) return nullptr;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snap==INVALID_HANDLE_VALUE) return nullptr;
    DWORD pid=GetCurrentProcessId();
    THREADENTRY32 te={sizeof(te)};
    JNIEnv* result=nullptr;
    int checked=0;
    static const uintptr_t TLS_SLOTS=0x1480, TLS_EXPANSION=0x1780;
    if(Thread32First(snap,&te)) do{
        if(te.th32OwnerProcessID!=pid) continue;
        checked++;
        HANDLE h=OpenThread(THREAD_QUERY_INFORMATION,FALSE,te.th32ThreadID);
        if(!h) continue;
        TBI tbi={}; LONG st=g_NtQIT(h,0,&tbi,sizeof(tbi),nullptr);
        CloseHandle(h);
        if(st||!tbi.TebBaseAddress) continue;
        uintptr_t teb=(uintptr_t)tbi.TebBaseAddress;
        __try{
            for(int i=0;i<64;i++){
                uintptr_t v=*((uintptr_t*)(teb+TLS_SLOTS)+i);
                JNIEnv* e=scan_thread_for_jnienv(v);
                if(e){result=e;goto done;}
            }
        }__except(1){}
        __try{
            uintptr_t exp=*(uintptr_t*)(teb+TLS_EXPANSION);
            if(exp>0x10000){
                for(int i=0;i<1024;i++){
                    uintptr_t v=*((uintptr_t*)exp+i);
                    JNIEnv* e=scan_thread_for_jnienv(v);
                    if(e){result=e;goto done;}
                }
            }
        }__except(1){}
    }while(Thread32Next(snap,&te));
done:
    CloseHandle(snap);
    con_log(WHITE,"[*] TEB scan: %d thread — env=%p\n",checked,result);
    return result;
}

static void jni_init_worker(){
    g_NtQIT=(pNtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationThread");

    con_log(WHITE,"[*] GL hook bekleniyor (max 60s)...\n");
    for(int tick=0;tick<1200&&!g_gl_ready.load();tick++){
        Sleep(50);
        if(tick>0 && tick%200==0)
            con_log(GREY,"[~] GL bekle... %ds/%ds\n",(tick*50)/1000,60);
    }
    if(g_gl_ready.load())
        con_log(GREEN,"[+] GL hazir\n");
    else
        con_log(YELLOW,"[!] GL hook timeout (60s) — wglSwapBuffers tetiklenmedi, devam ediliyor\n");

    HMODULE hJvm=nullptr;
    for(int i=0;i<120&&!hJvm;i++){hJvm=GetModuleHandleA("jvm.dll");if(!hJvm)Sleep(500);}
    if(!hJvm){con_log(RED,"[!] jvm.dll bulunamadi\n");return;}
    con_log(GREEN,"[+] jvm.dll: %p\n",hJvm);
    GetModuleInformation(GetCurrentProcess(),hJvm,&g_jvm_mi,sizeof(g_jvm_mi));
    con_log(WHITE,"[*] jvm.dll: %p - %p\n",
        g_jvm_mi.lpBaseOfDll,(void*)((uintptr_t)g_jvm_mi.lpBaseOfDll+g_jvm_mi.SizeOfImage));

    g_hJvm=hJvm;
    con_log(WHITE,"[*] JDK8u51 offset: 0x%llX -> %p\n",
        (unsigned long long)JDK8U51_GET_ENV_OFFSET,
        (void*)((uintptr_t)hJvm+JDK8U51_GET_ENV_OFFSET));
    con_log(WHITE,"[*] Render thread getter bekleniyor...\n");

    for(int w=0;w<100&&!g_env_ready.load();w++) Sleep(50);
    if(g_env_ready.load()){con_log(GREEN,"[+] Render thread basarili — done\n");return;}

    con_log(YELLOW,"[!] Render thread getter basarisiz — TEB scan fallback\n");
    g_init_started.store(true);

    JNIEnv* env=nullptr;
    for(int attempt=0;attempt<6&&!env;attempt++){
        env=teb_scan_all_threads();
        if(!env){con_log(GREY,"[~] TEB attempt %d/6\n",attempt+1);Sleep(1000);}
    }

    if(!env){
        con_log(YELLOW,"[*] JNI_GetCreatedJavaVMs fallback...\n");
        using GetVMs_t=jint(JNICALL*)(JavaVM**,jsize,jsize*);
        auto fn=(GetVMs_t)GetProcAddress(hJvm,"JNI_GetCreatedJavaVMs");
        if(fn){
            JavaVM* jvm=nullptr; jsize cnt=0;
            if(fn(&jvm,1,&cnt)==JNI_OK&&cnt>0&&jvm){
                JavaVMAttachArgs args={}; args.version=JNI_VERSION_1_8;
                args.name=const_cast<char*>("CRDumper");
                jint rc=jvm->AttachCurrentThread((void**)&env,&args);
                con_log(WHITE,"[*] Attach rc=%d env=%p\n",rc,env);
            }
        }
    }

    if(!env){
        con_log(YELLOW,"[*] JDK8u51 getter worker thread'den...\n");
        for(int i=0;i<10&&!env;i++){
            __try{
                auto fn=(tInternalGetEnv)((uintptr_t)hJvm+JDK8U51_GET_ENV_OFFSET);
                jint rc=fn(nullptr,(void**)&env,JNI_VERSION_1_8);
                con_log(WHITE,"[~] getter attempt %d: rc=%d env=%p\n",i+1,rc,env);
            }__except(EXCEPTION_EXECUTE_HANDLER){env=nullptr;}
            if(!env) Sleep(500);
        }
    }

    if(!env){con_log(RED,"[!] JNIEnv* alinamadi — tum yollar bitti\n");return;}
    con_log(GREEN,"[+] JNIEnv* fallback: %p\n",env);
    g_env_ready.store(true);
    initialize_and_dump(env);
}

static void init_console(){
    AllocConsole(); SetConsoleTitleA("CRNativeDumper — Craftrise JVM Mapper");
    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
    FILE* d;
    freopen_s(&d,"CONOUT$","w",stdout);
    freopen_s(&d,"CONOUT$","w",stderr);
    freopen_s(&d,"CONIN$","r",stdin);
    HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if(GetConsoleScreenBufferInfo(h,&csbi)){COORD sz=csbi.dwSize;sz.Y=9999;SetConsoleScreenBufferSize(h,sz);}
    g_con=h;
    SetConsoleTextAttribute(g_con,GREEN);
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║       CRNativeDumper  —  Craftrise JVM Mapper        ║\n");
    printf("║                   --By cmmdx256                     ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
    SetConsoleTextAttribute(g_con,WHITE);
}

void run_dumper(){
    make_output_dir();

    {
        char dbg_path[MAX_PATH] = {};
        _snprintf_s(dbg_path, sizeof(dbg_path), "%s\\CRDumper_debug.log", g_outdir);
        g_dbg = _fsopen(dbg_path, "w", _SH_DENYNO);
    }

    init_console();
    con_log(WHITE,"[*] Inject edildi — opengl32.dll bekleniyor...\n");

    HMODULE hGL=nullptr;
    for(int i=0;i<120&&!hGL;i++){hGL=GetModuleHandleA("opengl32.dll");if(!hGL)Sleep(500);}
    if(!hGL){con_log(RED,"[!] opengl32.dll bulunamadi\n");return;}
    con_log(GREEN,"[+] opengl32.dll bulundu\n");

    MH_STATUS mhs=MH_Initialize();
    if(mhs!=MH_OK&&mhs!=MH_ERROR_ALREADY_INITIALIZED){con_log(RED,"[!] MinHook: %d\n",(int)mhs);return;}
    con_log(GREEN,"[+] MinHook init OK\n");

    orig_wglSwapBuffers=(wglSwapBuffers_t)GetProcAddress(hGL,"wglSwapBuffers");
    if(!orig_wglSwapBuffers){con_log(RED,"[!] wglSwapBuffers bulunamadi\n");return;}
    if(MH_CreateHook((LPVOID)orig_wglSwapBuffers,(LPVOID)hooked_wglSwapBuffers,(LPVOID*)&orig_wglSwapBuffers)!=MH_OK){
        con_log(RED,"[!] hook basarisiz\n"); return;
    }
    MH_EnableHook(MH_ALL_HOOKS);
    con_log(GREEN,"[+] wglSwapBuffers hook aktif\n");

    HANDLE h=CreateThread(nullptr,0,[](LPVOID)->DWORD{jni_init_worker();return 0;},nullptr,0,nullptr);
    if(h) CloseHandle(h);

    while(true){
        Sleep(3000);
        std::lock_guard<std::mutex> lk(g_mutex);
        if(g_file){fflush(g_file);set_col(GREY);printf("[~] Toplam: %d class\r",g_class_count.load());set_col(WHITE);}
    }
}

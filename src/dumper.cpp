
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include "dumper.h"
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <gl/GL.h>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <mutex>
#include <set>
#include <atomic>
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

static FILE*  g_file     = nullptr;
static FILE*  g_fmt_file = nullptr;   
static HANDLE g_con  = INVALID_HANDLE_VALUE;
static std::mutex            g_mutex;
static std::set<std::string> g_written;
static std::atomic<int>      g_class_count{ 0 };

typedef void(__stdcall* wglSwapBuffers_t)(HDC);
static wglSwapBuffers_t  orig_wglSwapBuffers = nullptr;
static std::atomic<bool> g_gl_ready{ false };
static std::atomic<bool> g_env_ready{ false };
static std::atomic<bool> g_dump_done{ false };

static const uintptr_t JDK8U51_GET_ENV_OFFSET = 0x144080;

static char g_outdir[MAX_PATH] = {};

static void make_output_dir(){
    const char* dir_c = "C:\\CRNativeDumperByCmmdx256";
    if(CreateDirectoryA(dir_c, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS){
        _snprintf_s(g_outdir, sizeof(g_outdir), "%s", dir_c);
        return;
    }
    char prof[MAX_PATH] = {};
    if(GetEnvironmentVariableA("USERPROFILE", prof, sizeof(prof))){
        char dp[MAX_PATH];
        _snprintf_s(dp, sizeof(dp), "%.220s\\Desktop\\CRNativeDumperByCmmdx256", prof);
        if(CreateDirectoryA(dp, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS){
            _snprintf_s(g_outdir, sizeof(g_outdir), "%s", dp);
            return;
        }
        _snprintf_s(g_outdir, sizeof(g_outdir), "%.240s\\Desktop", prof);
    }
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

static void dump_class_fmt(JNIEnv* env, jclass klass, const std::string& dotname){
    if(!g_fmt_file) return;
    if(!g_jmid_getDeclaredFields && !g_jmid_getDeclaredMethods) return;

    std::string simple_name = dotname;
    { size_t p = dotname.rfind('.'); if(p != std::string::npos) simple_name = dotname.substr(p+1); }

    fprintf(g_fmt_file, "\n%s {\n  class: %s\n\n  fields: {\n", simple_name.c_str(), dotname.c_str());

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
                if(g_jmid_fldMods){
                    mods = env->CallIntMethod(fobj, g_jmid_fldMods);
                    raw_ExceptionClear(env);
                }
                bool is_static = (mods & 0x8) != 0;

                jlong offset = -1;
                if(g_unsafe && fobj){
                    jmethodID mid = is_static ? g_jmid_unsafeSFO : g_jmid_unsafeOFO;
                    if(mid){
                        offset = env->CallLongMethod(g_unsafe, mid, fobj);
                        raw_ExceptionClear(env);
                    }

                }

                if(!fname.empty()){
                    if(offset >= 0)
                        fprintf(g_fmt_file,
                            "    %s -> type: %s, unsafe-offset: 0x%llX (dec %lld)%s\n",
                            fname.c_str(), ftype.c_str(),
                            (unsigned long long)offset, (long long)offset,
                            is_static ? " [static]" : "");
                    else
                        fprintf(g_fmt_file,
                            "    %s -> type: %s%s\n",
                            fname.c_str(), ftype.c_str(),
                            is_static ? " [static]" : "");
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
                    if(parr){
                        jint plen = env->GetArrayLength((jarray)parr);
                        for(jint j = 0; j < plen; j++){
                            jobject pcls = env->GetObjectArrayElement((jobjectArray)parr, j);
                            raw_ExceptionClear(env);
                            if(j > 0) params_str += ", ";
                            if(pcls){ params_str += get_type_name(env, pcls); raw_DeleteLocalRef(env, pcls); }
                        }
                        raw_DeleteLocalRef(env, parr);
                    }
                }

                jint mmods = 0;
                if(g_jmid_mthMods){
                    mmods = env->CallIntMethod(mobj, g_jmid_mthMods);
                    raw_ExceptionClear(env);
                }
                bool m_static = (mmods & 0x8) != 0;

                if(!mname.empty())
                    fprintf(g_fmt_file, "    %s(%s) -> returns: %s%s\n",
                        mname.c_str(), params_str.c_str(), ret_type.c_str(),
                        m_static ? " [static]" : "");

                raw_DeleteLocalRef(env, mobj);
            }
            raw_DeleteLocalRef(env, marr);
        }
        raw_ExceptionClear(env);
    }

    fprintf(g_fmt_file, "  }\n}\n");
    fflush(g_fmt_file);
}

static void dump_class(jclass klass){
    if(!g_jmid_getName) return;

    jstring jname = (jstring)raw_CallObjectMethod(g_env, klass, g_jmid_getName);
    if(!jname || raw_ExceptionCheck(g_env)){ raw_ExceptionClear(g_env); return; }
    std::string dotname = jstr_to_std(g_env, jname);
    raw_DeleteLocalRef(g_env, jname);
    if(dotname.empty()) return;
    
    if(dotname[0] == '[') return;

    std::string sig = "L";
    for(char c : dotname) sig += (c == '.' ? '/' : c);
    sig += ";";

    {std::lock_guard<std::mutex> lk(g_mutex); if(g_written.count(sig)) return; g_written.insert(sig);}
    int idx = ++g_class_count;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        set_col(YELLOW); printf("[%5d] ", idx);
        set_col(CYAN);   printf("CLASS ");
        set_col(GREEN);  printf("%s\n", sig.c_str()); set_col(WHITE);
        if(g_file) fprintf(g_file, "CLASS %s\n", sig.c_str());
    }
    
    dump_class_fmt(g_env, klass, dotname);

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

    {std::lock_guard<std::mutex> lk(g_mutex);
     printf("\n");
     if(g_file){ fprintf(g_file, "\n"); fflush(g_file); }
    }
}

static void JNICALL class_file_load_hook(
    jvmtiEnv*, JNIEnv*, jclass, jobject, const char* name,
    jobject, jint, const unsigned char*, jint*, unsigned char**)
{
    if(!name) return;
    std::string sig = "L"; sig += name; sig += ";";
    {std::lock_guard<std::mutex> lk(g_mutex); if(g_written.count(sig)) return; g_written.insert(sig);}
    int idx = ++g_class_count;
    std::lock_guard<std::mutex> lk(g_mutex);
    set_col(YELLOW); printf("[%5d] ", idx);
    set_col(CYAN); printf("HOOK  "); set_col(GREEN); printf("%s\n", sig.c_str()); set_col(WHITE);
    if(g_file){ fprintf(g_file, "CLASS %s\n\n", sig.c_str()); fflush(g_file); }
}

static void do_snapshot(){
    jint cnt = 0; jclass* cls = nullptr;
    jvmtiError err = g_jvmti->GetLoadedClasses(&cnt, &cls);
    if(err != JVMTI_ERROR_NONE || !cls){
        con_log(RED, "[!] GetLoadedClasses err=%d\n", (int)err); return;
    }
    con_log(WHITE, "[*] Snapshot: %d class\n\n", cnt);
    for(jint i = 0; i < cnt; i++) dump_class(cls[i]);
    g_jvmti->Deallocate((unsigned char*)cls);
    con_log(GREEN, "\n[+] Snapshot tamam — %d class dump edildi\n\n", g_class_count.load());
    if(g_file) fflush(g_file);
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
    caps.can_retransform_classes = 1;
    caps.can_retransform_any_class = 1;
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
    printf("\n");

    if(cap_err == JVMTI_ERROR_NONE){
        jvmtiEventCallbacks cbs = {}; cbs.ClassFileLoadHook = class_file_load_hook;
        jvmti->SetEventCallbacks(&cbs, (jint)sizeof(cbs));
        jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr);
        con_log(WHITE, "[*] ClassFileLoadHook aktif\n\n");
    }

    do_snapshot();

    con_log(GREEN, "\n--By cmmdx256\n\n");
    if(g_file)    { fprintf(g_file,     "\n--By cmmdx256\n"); fflush(g_file); }
    if(g_fmt_file){ fprintf(g_fmt_file, "\n--By cmmdx256\n"); fflush(g_fmt_file); }
    if(g_dbg)     { fprintf(g_dbg,      "\n--By cmmdx256\n"); fflush(g_dbg); }

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

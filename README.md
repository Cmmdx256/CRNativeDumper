# CRNativeDumper

> **By cmmdx256**

---

## 🇬🇧 English

### What is it?

CRNativeDumper is a native x64 DLL that is injected into the **Craftrise** Minecraft client process (`craftrise-x64.exe`) to dump a complete mapping of all loaded JVM classes at runtime.

It uses a combination of **JVMTI** (JVM Tool Interface) and **JNI** (Java Native Interface) reflection to extract:

- Every loaded class with its fully-qualified obfuscated name
- All declared fields with their **types**, **unsafe memory offsets**, and **static flags**
- All declared methods with their **parameter types**, **return types**, and **static flags**
- A raw `CLASS / FIELD / METHOD` format for quick scripting
- A formatted block-style mapping compatible with classic Craftrise mapping layouts

---

### Features

| Feature | Detail |
|---|---|
| **Full class dump** | All ~8100+ JVM classes loaded in the Craftrise process |
| **Unsafe field offsets** | `sun.misc.Unsafe.objectFieldOffset` / `staticFieldOffset` per field |
| **Method signatures** | Full parameter type list + return type via JNI reflection |
| **Static detection** | `java.lang.reflect.Modifier.STATIC` flag on every field and method |
| **Two output formats** | Raw (`mappings.txt`) + Formatted (`mappings_fmt.txt`) |
| **Debug log** | Full runtime log in `CRDumper_debug.log` |
| **GL hook** | Hooks `wglSwapBuffers` to obtain a valid render-thread `JNIEnv*` |
| **TEB scan fallback** | Scans all thread TLS slots to locate `JNIEnv*` if GL hook misses |
| **Shared file access** | Output files can be read while writing (no lock) |

---

### Output Format

**`mappings.txt`** — Raw format:
```
CLASS Lnet/minecraft/client/Minecraft;
  FIELD theMinecraft Lnet/minecraft/client/Minecraft;
  METHOD getMinecraft ()Lnet/minecraft/client/Minecraft;
```

**`mappings_fmt.txt`** — Formatted block style:
```
Minecraft {
  class: net.minecraft.client.Minecraft

  fields: {
    theMinecraft -> type: net.minecraft.client.Minecraft, unsafe-offset: 0x68 (dec 104) [static]
    timer -> type: net.minecraft.util.Timer, unsafe-offset: 0x28 (dec 40)
  }

  methods: {
    getMinecraft() -> returns: net.minecraft.client.Minecraft [static]
    runGameLoop() -> returns: void
    sendClickBlockToController(net.minecraft.util.math.BlockPos, net.minecraft.util.EnumFacing) -> returns: void
  }
}
```

---

### Output Location

All files are saved to:

```
C:\CRNativeDumperByCmmdx256\
    mappings.txt
    mappings_fmt.txt
    CRDumper_debug.log
```

If writing to `C:\` fails (permissions), the folder is created on the Desktop instead:

```
%USERPROFILE%\Desktop\CRNativeDumperByCmmdx256\
```

---

### Requirements

| Requirement | Version |
|---|---|
| Target process | `craftrise-x64.exe` (Craftrise Minecraft client) |
| JDK | JDK 1.8.0_51 (Craftrise bundled JDK at `%AppData%\.craftrise\java\jdk-x64\`) |
| Build toolchain | Visual Studio 2022 (MSVC v143), x64 |
| MinHook | Included in `include/` |
| OpenGL32 | System (`opengl32.lib`) |

---

### Build

```powershell
# Open Developer Command Prompt or use MSBuild directly:
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    CRNativeDumper.vcxproj /p:Configuration=Release /p:Platform=x64

# Output:
# bin_out\x64\Release\CRNativeDumper.dll
```

---

### Inject

```powershell
# Start Craftrise and wait for it to load fully, then:
.\injector\out\injector.exe
```

The injector will locate the `craftrise-x64.exe` process and inject `CRNativeDumper.dll`.

A console window will open showing live progress. Dump completes automatically — watch for:

```
[+] Snapshot tamam — 8107 class dump edildi
--By cmmdx256
```

---

### How It Works

```
DllMain → run_dumper()
    │
    ├─ make_output_dir()         Create CRNativeDumperByCmmdx256 folder
    ├─ init_console()            Allocate debug console
    ├─ Hook wglSwapBuffers       Get render-thread JNIEnv*
    │
    └─ jni_init_worker() [thread]
          ├─ Wait for GL hook (max 60s)
          ├─ Wait for jvm.dll
          ├─ JDK8u51 internal getter → JNIEnv* (render thread, most reliable)
          ├─ Fallback: TEB scan all threads
          ├─ Fallback: JNI_GetCreatedJavaVMs + AttachCurrentThread
          │
          └─ initialize_and_dump(env)
                ├─ GetJavaVM → JavaVM*
                ├─ GetEnv → jvmtiEnv*
                ├─ init_jni_pointers()       JNI vtable extraction (fixed indices)
                ├─ init_jni_reflection()     Class/Field/Method/Unsafe method IDs
                ├─ Open output files
                │
                └─ do_snapshot()
                      └─ GetLoadedClasses → dump_class() × ~8100
                            ├─ JVMTI GetClassFields  → field names + descriptors
                            ├─ JNI  getDeclaredFields → types + Unsafe offsets + static flags
                            ├─ JVMTI GetClassMethods → method names
                            ├─ JNI  getDeclaredMethods → return types + params + static flags
                            ├─ Write mappings.txt  (raw)
                            └─ Write mappings_fmt.txt (formatted)
```

---

### Project Structure

```
CRNativeDumper/
├─ src/
│   ├─ dumper.cpp       Main DLL logic (inject, hook, dump)
│   ├─ dumper.h         Exported entry point declaration
│   └─ dllmain.cpp      DLL entry point → calls run_dumper() on new thread
├─ injector/
│   ├─ injector.cpp     Standalone injector executable
│   └─ out/
│       └─ injector.exe Pre-built injector
├─ include/
│   ├─ jni.h            JNI header (JDK 8)
│   ├─ jni_md.h
│   ├─ jvmti.h          JVMTI header (JDK 8)
│   ├─ MinHook.h        MinHook API
│   └─ unhook.h
├─ bin_out/
│   └─ x64/Release/
│       └─ CRNativeDumper.dll
└─ CRNativeDumper.vcxproj
```

---

---

## 🇹🇷 Türkçe

### Nedir?

CRNativeDumper, **Craftrise** Minecraft istemci sürecine (`craftrise-x64.exe`) enjekte edilen, çalışma zamanında yüklü tüm JVM sınıflarının tam bir mapping'ini çıkaran yerel bir x64 DLL'dir.

**JVMTI** (JVM Tool Interface) ve **JNI** (Java Native Interface) yansıma mekanizmalarını birlikte kullanarak şunları çıkarır:

- Tüm yüklü sınıflar — tam nitelikli, obfuscate edilmiş adlarıyla
- Tüm alanlar — **tipler**, **unsafe bellek offsetleri** ve **static bayrakları** ile
- Tüm metotlar — **parametre tipleri**, **dönüş tipleri** ve **static bayrakları** ile
- Hızlı script kullanımı için ham `CLASS / FIELD / METHOD` formatı
- Klasik Craftrise mapping düzeniyle uyumlu biçimlendirilmiş blok stili çıktı

---

### Özellikler

| Özellik | Detay |
|---|---|
| **Tam sınıf dökümü** | Craftrise sürecinde yüklü ~8100+ JVM sınıfının tamamı |
| **Unsafe alan offsetleri** | Her alan için `sun.misc.Unsafe.objectFieldOffset` / `staticFieldOffset` |
| **Metot imzaları** | JNI yansıma ile tam parametre listesi + dönüş tipi |
| **Static tespiti** | Her alan ve metotta `java.lang.reflect.Modifier.STATIC` bayrağı |
| **İki çıktı formatı** | Ham (`mappings.txt`) + Biçimlendirilmiş (`mappings_fmt.txt`) |
| **Debug logu** | Tam çalışma zamanı logu `CRDumper_debug.log` dosyasında |
| **GL hook** | Geçerli bir render-thread `JNIEnv*` elde etmek için `wglSwapBuffers` hook'u |
| **TEB scan yedek yolu** | GL hook başarısız olursa tüm thread TLS slotları taranır |
| **Paylaşımlı dosya erişimi** | Çıktı dosyaları yazılırken okunabilir (kilit yok) |

---

### Çıktı Formatı

**`mappings.txt`** — Ham format:
```
CLASS Lnet/minecraft/client/Minecraft;
  FIELD theMinecraft Lnet/minecraft/client/Minecraft;
  METHOD getMinecraft ()Lnet/minecraft/client/Minecraft;
```

**`mappings_fmt.txt`** — Biçimlendirilmiş blok stili:
```
Minecraft {
  class: net.minecraft.client.Minecraft

  fields: {
    theMinecraft -> type: net.minecraft.client.Minecraft, unsafe-offset: 0x68 (dec 104) [static]
    timer -> type: net.minecraft.util.Timer, unsafe-offset: 0x28 (dec 40)
  }

  methods: {
    getMinecraft() -> returns: net.minecraft.client.Minecraft [static]
    runGameLoop() -> returns: void
    sendClickBlockToController(net.minecraft.util.math.BlockPos, net.minecraft.util.EnumFacing) -> returns: void
  }
}
```

---

### Çıktı Konumu

Tüm dosyalar şuraya kaydedilir:

```
C:\CRNativeDumperByCmmdx256\
    mappings.txt
    mappings_fmt.txt
    CRDumper_debug.log
```

`C:\` dizinine yazma başarısız olursa (yetki hatası), klasör bunun yerine Masaüstünde oluşturulur:

```
%USERPROFILE%\Desktop\CRNativeDumperByCmmdx256\
```

---

### Gereksinimler

| Gereksinim | Sürüm |
|---|---|
| Hedef süreç | `craftrise-x64.exe` (Craftrise Minecraft istemcisi) |
| JDK | JDK 1.8.0_51 (Craftrise dahili JDK: `%AppData%\.craftrise\java\jdk-x64\`) |
| Derleme araç zinciri | Visual Studio 2022 (MSVC v143), x64 |
| MinHook | `include/` klasöründe mevcut |
| OpenGL32 | Sistem (`opengl32.lib`) |

---

### Derleme

```powershell
# Developer Command Prompt'u aç veya MSBuild'i doğrudan kullan:
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    CRNativeDumper.vcxproj /p:Configuration=Release /p:Platform=x64

# Çıktı:
# bin_out\x64\Release\CRNativeDumper.dll
```

---

### Enjekte Etme

```powershell
# Craftrise'ı başlat, tamamen yüklenmesini bekle, sonra:
.\injector\out\injector.exe
```

Injector `craftrise-x64.exe` sürecini bulur ve `CRNativeDumper.dll`'i enjekte eder.

Canlı ilerlemeyi gösteren bir konsol penceresi açılır. Dump otomatik tamamlanır — şunu bekle:

```
[+] Snapshot tamam — 8107 class dump edildi
--By cmmdx256
```

---

### Nasıl Çalışır?

```
DllMain → run_dumper()
    │
    ├─ make_output_dir()         CRNativeDumperByCmmdx256 klasörünü oluştur
    ├─ init_console()            Debug konsolunu aç
    ├─ wglSwapBuffers hook'u     Render-thread JNIEnv* al
    │
    └─ jni_init_worker() [thread]
          ├─ GL hook bekle (max 60s)
          ├─ jvm.dll'i bekle
          ├─ JDK8u51 dahili getter → JNIEnv* (render thread, en güvenilir yol)
          ├─ Yedek: TEB scan tüm thread'ler
          ├─ Yedek: JNI_GetCreatedJavaVMs + AttachCurrentThread
          │
          └─ initialize_and_dump(env)
                ├─ GetJavaVM → JavaVM*
                ├─ GetEnv → jvmtiEnv*
                ├─ init_jni_pointers()       JNI vtable çıkarma (sabit indeksler)
                ├─ init_jni_reflection()     Class/Field/Method/Unsafe metot ID'leri
                ├─ Çıktı dosyalarını aç
                │
                └─ do_snapshot()
                      └─ GetLoadedClasses → dump_class() × ~8100
                            ├─ JVMTI GetClassFields  → alan adları + tanımlayıcılar
                            ├─ JNI  getDeclaredFields → tipler + Unsafe offsetleri + static bayrakları
                            ├─ JVMTI GetClassMethods → metot adları
                            ├─ JNI  getDeclaredMethods → dönüş tipleri + parametreler + static bayrakları
                            ├─ mappings.txt yaz  (ham)
                            └─ mappings_fmt.txt yaz (biçimlendirilmiş)
```

---

### Proje Yapısı

```
CRNativeDumper/
├─ src/
│   ├─ dumper.cpp       Ana DLL mantığı (inject, hook, dump)
│   ├─ dumper.h         Dışa aktarılan giriş noktası bildirimi
│   └─ dllmain.cpp      DLL giriş noktası → yeni thread'de run_dumper() çağrısı
├─ injector/
│   ├─ injector.cpp     Bağımsız injector çalıştırılabiliri
│   └─ out/
│       └─ injector.exe Hazır injector
├─ include/
│   ├─ jni.h            JNI başlık dosyası (JDK 8)
│   ├─ jni_md.h
│   ├─ jvmti.h          JVMTI başlık dosyası (JDK 8)
│   ├─ MinHook.h        MinHook API
│   └─ unhook.h
├─ bin_out/
│   └─ x64/Release/
│       └─ CRNativeDumper.dll
└─ CRNativeDumper.vcxproj
```

---

**--By cmmdx256**

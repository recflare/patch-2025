// PreserveLoader.dll -- Rec Room Preservation proxy loader.
//
// Rec Room statically imports exactly one module, "Referee.dll" (the Themida
// protector; it exports a single obfuscated gate, "xVnMnf"). A proxy under that
// name is a perfectly ordinary way to get an ordinary, first-class foothold in
// the game process: the game binds itself to this module at process-init time,
// no remote threads, no cross-process memory writes, no suspended threads.
//
// What this loader does:
//   1. DllMain loads the REAL protector (shipped separately as preserve\Referee.dll,
//      falling back to RefereeOriginal.dll in the game root) and resolves its
//      xVnMnf export. The client proceeds through the trampoline in xVnMnf.asm,
//      which tail-jumps to the real export. The protector runs exactly as before.
//   2. GetPreserveProtector() is exported so 2025Patch (or any preservation tooling)
//      can find the protector's load base even though "Referee.dll" in the module
//      list is this proxy. 2025Patch uses it to base its protector hooks on the
//      real module, not this one.
//   3. A watcher thread waits for GameAssembly.dll to finish loading, then loads
//      EVERY *.dll found in preserve\modules\ -- the folder IS the mod system.
//      Load order is alphabetical (sort by filename), so modules that need to run
//      first use a numeric prefix (e.g. 0000-core.dll). 2025Patch.dll lives in
//      that folder too, next to anything else you drop in. A module that fails to
//      load is logged and skipped; it never blocks the rest.
//
// Layout after Install-PreserveLoader.bat:
//   Referee.dll                    <- this loader (xVnMnf export)
//   preserve\Referee.dll           <- the real protector, untouched
//   preserve\modules\*.dll         <- the mods (2025Patch.dll among them)
//
// Revert at any time with Restore-RefereeProtector.bat.
//
// Rules:
//   - Never do real work in DllMain beyond loading the protector; everything else
//     happens on the watcher thread.
//   - Never touch il2cpp exports from here.
//   - Missing 2025Patch.dll logs and leaves the game running unpatched rather
//     than killing it.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <wchar.h>
#include <string>
#include <vector>
#include <algorithm>

// Jump target for the xVnMnf trampoline (xVnMnf.asm): the protector's real
// export, resolved at DllMain time from the protector we load.
extern "C" ULONGLONG g_realXNfn = 0;

static HMODULE g_original = nullptr; // never freed; the protector stays resident

extern "C" __declspec(dllexport) void* GetPreserveProtector(void) {
    // Used by the patch (and analysis tooling) to find the real protector base
    // when this proxy is what the loader registered as "Referee.dll".
    return (void*)g_original;
}

static void GetModuleDir(HMODULE module, wchar_t* out, size_t outCount) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
    wcsncpy_s(out, outCount, path, _TRUNCATE);
}

static void Log(const char* fmt, ...) {
    wchar_t dir[MAX_PATH] = {};
    GetModuleDir(nullptr, dir, MAX_PATH);
    wchar_t logPath[MAX_PATH + 80] = {};
    _snwprintf_s(logPath, _countof(logPath), _TRUNCATE, L"%s\\preserve-loader.log", dir);

    FILE* f = nullptr;
    _wfopen_s(&f, logPath, L"a");
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    fclose(f);
}

static DWORD WINAPI BootstrapThread(LPVOID) {
    // Wait for the protector to have unpacked the client and GameAssembly.dll
    // to appear as a loaded module. ~2 minutes of polling at 50ms is far past
    // any realistic load time on this build.
    for (int i = 0; i < 2400; ++i) {
        if (GetModuleHandleW(L"GameAssembly.dll")) {
            // Give the runtime a beat to bring its exports up; the patch fails
            // closed on any missing export anyway.
            Sleep(300);

            wchar_t dir[MAX_PATH] = {};
            GetModuleDir(nullptr, dir, MAX_PATH);
            std::wstring modulesDir = std::wstring(dir) + L"\\preserve\\modules";
            std::wstring pattern = modulesDir + L"\\*.dll";

            // Gather the module list first so load order is deterministic:
            // alphabetical by filename. Numeric prefixes control order.
            WIN32_FIND_DATAW fd = {};
            HANDLE search = FindFirstFileW(pattern.c_str(), &fd);
            std::vector<std::wstring> modules;
            if (search != INVALID_HANDLE_VALUE) {
                do {
                    modules.emplace_back(fd.cFileName);
                } while (FindNextFileW(search, &fd));
                FindClose(search);
            }
            std::sort(modules.begin(), modules.end(), [](const std::wstring& a, const std::wstring& b) {
                return _wcsicmp(a.c_str(), b.c_str()) < 0;
            });

            if (modules.empty()) {
                Log("no modules found in %ls; client is running unpatched", modulesDir.c_str());
                return 0;
            }

            for (const auto& name : modules) {
                std::wstring full = modulesDir + L"\\" + name;
                HMODULE m = LoadLibraryW(full.c_str());
                if (m) {
                    Log("loaded %ls (module=%p)", name.c_str(), (void*)m);
                } else {
                    Log("FAILED to load %ls (error %lu); continuing", name.c_str(), GetLastError());
                }
            }
            return 0;
        }
        Sleep(50);
    }
    Log("timed out waiting for GameAssembly.dll; modules not attached");
    return 1;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        // No TLS callbacks, no extra thread attach/detach storms.
        DisableThreadLibraryCalls(hModule);
        Log("proxy attached; resolving the protector");

        wchar_t dir[MAX_PATH] = {};
        GetModuleDir(nullptr, dir, MAX_PATH);
        std::wstring subPath = std::wstring(dir) + L"\\preserve\\Referee.dll";
        std::wstring rootPath = std::wstring(dir) + L"\\RefereeOriginal.dll";

        g_original = LoadLibraryW(subPath.c_str());
        if (!g_original) {
            Log("preserve\\Referee.dll not loadable (error %lu); trying RefereeOriginal.dll",
                GetLastError());
            g_original = LoadLibraryW(rootPath.c_str());
        }
        if (!g_original) {
            Log("FATAL: protector not found or failed to load (error %lu)", GetLastError());
            return FALSE; // fail closed; the client cannot run without its protector
        }
        FARPROC real = GetProcAddress(g_original, "xVnMnf");
        if (!real) {
            Log("FATAL: xVnMnf not exported by the protector (error %lu)", GetLastError());
            return FALSE;
        }
        g_realXNfn = (ULONGLONG)real;
        Log("protector resolved (module=%p xVnMnf=%p)", (void*)g_original, (void*)real);

        HANDLE watcher = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
        if (watcher) {
            CloseHandle(watcher);
        } else {
            Log("CreateThread failed (error %lu); patch not attached", GetLastError());
        }
    }
    return TRUE;
}

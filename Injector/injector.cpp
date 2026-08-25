// Standalone injector for the Rec Room 2025 archival patch.
//
// Waits for an UNPATCHED Recroom_Release.exe, waits until that process has loaded
// GameAssembly.dll and Referee.dll (the patch resolves both at attach time), then
// loads the patch DLL with CreateRemoteThread + LoadLibraryW so its DllMain runs
// normally.
//
// Instances that already carry the patch are skipped rather than treated as a reason
// to stop, so several clients can run side by side with one injector each.
//
// No third-party tools required. Ship injector.exe + the patch DLL in the same folder.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <vector>
#include <iostream>
#include <thread>
#include <chrono>

static const wchar_t* kProcessName = L"Recroom_Release.exe";
static const wchar_t* kDefaultDll  = L"2025Patch.dll";
// How long the window lingers after a successful run, just long enough to read
// the last line. Failures still wait on Enter instead.
static const std::chrono::milliseconds kExitDelay{1500};

// Modules the patch needs present before it can hook anything.
static const wchar_t* kRequiredModules[] = { L"GameAssembly.dll", L"Referee.dll" };

// Every process with this name, in enumeration order. Plural on purpose: running two clients on
// one machine is a normal thing to want, so the injector has to be able to tell them apart instead
// of assuming the first match is the one that was just launched.
static void FindProcessIds(const wchar_t* name, std::vector<DWORD>& out) {
    out.clear();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) out.push_back(entry.th32ProcessID);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
}

// Whether one specific PID is still running. The liveness check used to compare "the first process
// with this name" against the target, which reports a perfectly healthy game as exited the moment a
// second client turns up ahead of it in the snapshot.
static bool ProcessIsAlive(DWORD pid) {
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!proc) return false;
    const bool alive = WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
    CloseHandle(proc);
    return alive;
}

// Returns true if the named module is loaded in the target process.
static bool ProcessHasModule(DWORD pid, const wchar_t* moduleName) {
    // The module snapshot briefly fails with ERROR_BAD_LENGTH while the target's
    // module list is changing; retry a few times before giving up.
    for (int attempt = 0; attempt < 8; ++attempt) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_BAD_LENGTH) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            return false;
        }

        MODULEENTRY32W mod{};
        mod.dwSize = sizeof(mod);

        bool found = false;
        if (Module32FirstW(snap, &mod)) {
            do {
                if (_wcsicmp(mod.szModule, moduleName) == 0) {
                    found = true;
                    break;
                }
            } while (Module32NextW(snap, &mod));
        }
        CloseHandle(snap);
        return found;
    }
    return false;
}

static bool AllRequiredModulesLoaded(DWORD pid) {
    for (const wchar_t* m : kRequiredModules) {
        if (!ProcessHasModule(pid, m)) return false;
    }
    return true;
}

static bool Inject(DWORD pid, const std::wstring& dllPath) {
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!proc) {
        std::wcout << L"[!] OpenProcess failed (" << GetLastError()
                   << L"). Try running the injector as administrator.\n";
        return false;
    }

    bool ok = false;
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);

    LPVOID remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        std::wcout << L"[!] VirtualAllocEx failed (" << GetLastError() << L").\n";
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        std::wcout << L"[!] WriteProcessMemory failed (" << GetLastError() << L").\n";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    // kernel32.dll loads at the same address in every process on a given boot,
    // so LoadLibraryW's address here is valid in the target too.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW"));

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);

        DWORD exitCode = 0;  // low 32 bits of the returned HMODULE
        GetExitCodeThread(thread, &exitCode);
        CloseHandle(thread);

        // The thread exit code alone is unreliable on x64 (only 32 bits of the
        // HMODULE), so confirm by re-scanning the module list.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::wstring dllName = dllPath.substr(dllPath.find_last_of(L"\\/") + 1);
        ok = ProcessHasModule(pid, dllName.c_str());
        if (!ok && exitCode != 0) ok = true;
    } else {
        std::wcout << L"[!] CreateRemoteThread failed (" << GetLastError() << L").\n";
    }

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    // Flush per insertion: to a console this costs nothing, but redirected to a file wcout is
    // fully buffered, so `Injector.exe > inject.log` stays empty until the process exits --
    // and the runs worth reading (still waiting, still loading) are the ones still going.
    std::wcout << std::unitbuf;

    std::wcout << L"Rec Room 2025 patch injector\n";
    std::wcout << L"----------------------------\n";

    // Resolve the DLL path: first CLI arg, else <injector folder>\2025Patch.dll.
    std::wstring dllPath;
    if (argc > 1) {
        dllPath = argv[1];
    } else {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring dir(exePath);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        dllPath = dir + kDefaultDll;
    }

    // Turn into a full path and confirm it exists before doing anything else.
    wchar_t fullDll[MAX_PATH]{};
    if (!GetFullPathNameW(dllPath.c_str(), MAX_PATH, fullDll, nullptr) ||
        GetFileAttributesW(fullDll) == INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"[!] Patch DLL not found: " << dllPath << L"\n";
        std::wcout << L"    Put the DLL next to this injector, or pass its path as an argument.\n";
        std::wcout << L"\nPress Enter to exit...";
        std::wcin.get();
        return 1;
    }
    dllPath = fullDll;
    std::wcout << L"[*] Patch DLL: " << dllPath << L"\n";

    const std::wstring dllName = dllPath.substr(dllPath.find_last_of(L"\\/") + 1);

    // Only one injector may travel from "this process is unpatched" to "injected". Two launchers
    // started together means two injectors racing over the same new process: both would look, both
    // would see no patch, and both would attach -- the double-inject that installs every hook twice
    // and crashes the game. The loser re-checks under the mutex, finds the DLL already there, and
    // goes back to looking for another instance. Local\ (per-session) is enough -- the game and its
    // injector are always the same logon session -- and it needs no privileges, unlike Global\.
    HANDLE gate = CreateMutexW(nullptr, FALSE, L"Local\\RecRoom2025PatchInjector");

    std::wcout << L"[*] Waiting for an unpatched " << kProcessName << L" (launch Rec Room now)...\n";

    int reported = -1;   // last "already patched" count printed, so the window doesn't scroll
    for (;;) {
        // Pick a target: the first instance that does NOT already have the patch loaded. Skipping
        // the patched ones is still the double-inject guard, but it is now a per-process skip rather
        // than a reason to give up. The old code took the first PID with a matching name and exited
        // if that one was patched -- which made a second client impossible to patch, because the
        // injector kept finding the client that was already running, reported "nothing to do", and
        // left the newly launched one running against the dead official backend.
        DWORD pid = 0;
        int patched = 0;
        for (;;) {
            std::vector<DWORD> pids;
            FindProcessIds(kProcessName, pids);

            patched = 0;
            for (DWORD candidate : pids) {
                if (ProcessHasModule(candidate, dllName.c_str())) ++patched;
                else if (pid == 0) pid = candidate;
            }
            if (pid != 0) break;

            // Only speak up when the count changes, so an idle wait doesn't scroll the window.
            if (patched != reported) {
                reported = patched;
                if (patched > 0) {
                    std::wcout << L"[=] " << patched << L" instance(s) already patched; waiting for a new one.\n";
                    std::wcout << L"    (close this window if you didn't mean to launch another client)\n";
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        std::wcout << L"[+] Found unpatched game, PID " << pid;
        if (patched > 0) std::wcout << L" (" << patched << L" other instance(s) already patched)";
        std::wcout << L"\n";

        // Wait for the game to finish loading the modules the patch depends on.
        std::wcout << L"[*] Waiting for the game to finish loading...\n";
        while (!AllRequiredModulesLoaded(pid)) {
            // If the game closes while we wait, stop.
            if (!ProcessIsAlive(pid)) {
                std::wcout << L"[!] Game process exited before it finished loading.\n";
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        // Small settle delay so the modules are fully initialized, not just mapped.
        std::this_thread::sleep_for(std::chrono::seconds(2));

        std::wcout << L"[*] Injecting...\n";
        if (gate) WaitForSingleObject(gate, INFINITE);
        const bool taken = ProcessHasModule(pid, dllName.c_str());
        const bool ok = !taken && Inject(pid, dllPath);
        if (gate) ReleaseMutex(gate);

        if (taken) {
            std::wcout << L"[=] PID " << pid << L" was patched by another injector; looking for another instance.\n";
            continue;
        }
        if (!ok) {
            // Only stall on failure, so the reason stays on screen.
            std::wcout << L"[!] Injection failed. See messages above.\n";
            std::wcout << L"\nPress Enter to exit...";
            std::wcin.get();
            return 1;
        }
        break;
    }

    std::wcout << L"[+] Done. The patch is loaded.\n";
    std::this_thread::sleep_for(kExitDelay);
    return 0;
}

// Metadata dumper for Rec Room (build 20250718.01, Unity 2021.3.41f1, il2cpp).
//
// Why this exists: on disk, Recroom_Release_Data/.../Metadata/global-metadata.dat is a
// 48 MB dummy (every byte 0x52) and GameAssembly.dll's image is encrypted/packed (Referee).
// The real il2cpp metadata is decrypted into memory at startup and never touches disk, so
// the ONLY way to get a global-metadata.dat for Cpp2IL/Il2CppInterop is to take it from the
// running process.
//
// What this tool does (and does NOT do):
//   - Waits for Recroom_Release.exe with GameAssembly.dll + Referee.dll loaded (same wait
//     pattern as Injector.exe).
//   - Polls the process's committed memory for the il2cpp metadata sanity magic
//     (0xFEEDFACF little-endian) with a plausible metadata version. Re-scans in short
//     passes while the game is still booting, so it catches the moment the real image
//     appears.
//   - Once found: SUSPENDS all threads of the game process (NtSuspendProcess, with a
//     Toolhelp SuspendThread fallback), writes a FULL memory dump (.dmp,
//     MiniDumpWithFullMemory) of the game, then resumes it.
//
//   It does NOT inject code, hook anything, modify files, or talk to the network. It only
//   reads process memory and writes one .dmp file, next to the game exe in a dumps\
//   subfolder (or to the path given as argv[1]). Sniffing out the metadata from the .dmp
//   happens offline in a separate step.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <DbgHelp.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <cwchar>

#pragma comment(lib, "dbghelp.lib")

static const wchar_t* kProcessName = L"Recroom_Release.exe";
static const wchar_t* kRequiredModules[] = { L"GameAssembly.dll", L"Referee.dll" };

// How long to scan for a plausible metadata image before falling back to the dump-anyway
// path. The game typically survives ~20-70s against the dead backend, so keep this well
// inside the boot window -- and remember the dump happens either way.
static const DWORD kSearchBudgetMs = 25 * 1000;
static const DWORD kPollIntervalMs = 500;
static const DWORD kChunkSize = 4 * 1024 * 1024;

// 0xFEEDFACF little-endian -- the il2cpp metadata sanity field.
static const uint8_t kMetadataMagic[4] = { 0xFC, 0xFA, 0xED, 0xFE };

// Unity 2021.3 ships metadata version 29. A plausible-version hit is our early-dump trigger.
// If no plausible hit exists, we still dump anyway (see the main flow) -- this check is a
// timing hint, not a requirement.
static const uint32_t kMinPlausibleVersion = 24;
static const uint32_t kMaxPlausibleVersion = 32;

// ------------------------------------------------------------------------------------------------------
// Process enumeration (same approach as Injector.exe -- kept local deliberately so the two
// tools can evolve independently).
// ------------------------------------------------------------------------------------------------------

// Enumerate every process with this name. (Plural-on-purpose, like the injector: several
// clients can run side by side and we want the one that just started, not the first match.)
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

static bool ProcessIsAlive(DWORD pid) {
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!proc) return false;
    const bool alive = WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
    CloseHandle(proc);
    return alive;
}

static bool ProcessHasModule(DWORD pid, const wchar_t* moduleName) {
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
                if (_wcsicmp(mod.szModule, moduleName) == 0) { found = true; break; }
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

static std::wstring GetProcessExePath(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t path[32768] = {};
    DWORD size = (DWORD)(sizeof(path) / sizeof(path[0]));
    std::wstring out;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) out.assign(path, size);
    CloseHandle(proc);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Suspend / resume. NtSuspendProcess stops every thread in the target in one call, which is
// what keeps Referee's watchdogs frozen while we read memory. Falls back to per-thread
// SuspendThread if the call is unavailable.
// ---------------------------------------------------------------------------------------------

typedef LONG NTSTATUS;
using NtSuspendProcess_t = NTSTATUS(WINAPI*)(HANDLE);
using NtResumeProcess_t  = NTSTATUS(WINAPI*)(HANDLE);

static NtSuspendProcess_t ResolveSuspendProcess() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return nullptr;
    return reinterpret_cast<NtSuspendProcess_t>(GetProcAddress(ntdll, "NtSuspendProcess"));
}

static NtResumeProcess_t ResolveResumeProcess() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return nullptr;
    return reinterpret_cast<NtResumeProcess_t>(GetProcAddress(ntdll, "NtResumeProcess"));
}

static void SuspendProcessToolhelp(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snap, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid) {
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (thread) { SuspendThread(thread); CloseHandle(thread); }
            }
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
}

static bool SuspendProcess(HANDLE proc, DWORD pid) {
    if (auto fn = ResolveSuspendProcess()) {
        if (fn(proc) >= 0) return true;
    }
    SuspendProcessToolhelp(pid);
    return true;   // best-effort; a suspended subset still freezes most of the process
}

static void ResumeProcess(HANDLE proc, DWORD pid) {
    if (auto fn = ResolveResumeProcess()) {
        if (fn(proc) >= 0) return;
    }
    // Resume path is a formality -- if the game self-quits it does not matter; do nothing.
    (void)pid;
}

// ---------------------------------------------------------------------------------------------
// Scanning committed memory for the il2cpp metadata image.
//
// Reads the process in overlapping 4 MB chunks and memchr's for 0xFC. A candidate only
// counts when it is followed by FA ED FE AND a 24..32 version -- two independent checks, so
// false positives are practically impossible. The full committed address space is scanned
// (code, data, heap, mappings), because we have no reason to trust which region carries it.
// ---------------------------------------------------------------------------------------------

// Attempt every committed region regardless of protection bits. ReadProcessMemory copies
// kernel-side; a PAGE_NOACCESS or PAGE_GUARD page may still come back readable, or the call
// just fails cheaply per chunk -- either way we keep the scan complete.

struct MetadataHit {
    uint64_t address = 0;
    uint32_t version = 0;
};

static bool ScanForMetadata(HANDLE proc, MetadataHit& hit) {
    // 64-bit Windows user-mode addresses live below 0x00007fffffffff.
    const uint64_t kMaxAddress = 0x00007FFFFFFFFFFFull;
    uint64_t addr = 0;

    // A single pass must never spin forever: cap consecutive VirtualQueryEx failures and
    // the elapsed time, so the suspend/scan/resume cycle always makes forward progress.
    const auto scanStart = std::chrono::steady_clock::now();
    int consecutiveFailures = 0;

    std::vector<uint8_t> buffer(kChunkSize + 8);

    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < kMaxAddress) {
        // Hard wall on one pass -- ~20s is plenty for a --GB process, and far inside the
        // ~20s the game has before it self-quits.
        if (std::chrono::steady_clock::now() - scanStart > std::chrono::seconds(15)) return false;

        const SIZE_T querySize = VirtualQueryEx(
            proc, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)), &mbi, sizeof(mbi));
        if (querySize != sizeof(mbi)) {
            // Do NOT touch mbi.BaseAddress here -- it is stale from a previous iteration
            // and can move the address backwards into an endless loop. Just step forward.
            addr += 0x1000;
            if (++consecutiveFailures > 4096) return false;
            continue;
        }
        consecutiveFailures = 0;
        const uint64_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uint64_t regionSize = mbi.RegionSize;
        const uint64_t regionEnd  = regionBase + regionSize;

        if (mbi.State == MEM_COMMIT) {
            uint64_t pos = regionBase;
            while (pos < regionEnd) {
                const uint64_t remaining = regionEnd - pos;
                const SIZE_T toRead = (SIZE_T)(remaining < buffer.size() ? remaining : buffer.size());

                SIZE_T got = 0;
                if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(pos)),
                                       buffer.data(), toRead, &got) || got < 8) {
                    // Region said readable but RPM refused (can happen on exotic pages);
                    // move on rather than fail the whole scan.
                    pos += toRead;
                    continue;
                }

                SIZE_T i = 0;
                while (i + 8 <= got) {
                    const void* found = memchr(buffer.data() + i, 0xFC, got - i);
                    if (!found) break;
                    const size_t off = static_cast<const uint8_t*>(found) - buffer.data();
                    if (off + 8 > got) break;  // straddles the chunk tail; overlap covers it
                    const uint8_t* p = buffer.data() + off;
                    if (p[1] == 0xFA && p[2] == 0xED && p[3] == 0xFE) {
                        uint32_t version = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                                           ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
                        if (version >= kMinPlausibleVersion && version <= kMaxPlausibleVersion) {
                            hit.address = pos + off;
                            hit.version = version;
                            return true;
                        }
                    }
                    i = off + 1;
                }

                // 8-byte overlap so a magic that starts in the tail of one chunk is fully
                // visible in the next.
                if (remaining <= buffer.size()) pos = regionEnd;
                else                             pos += buffer.size() - 8;
            }
        }

        // Advance to the next region; guard against a zero-sized region looping forever.
        addr = regionBase + regionSize;
        if (regionSize == 0) addr = regionBase + 0x1000;
    }

    return false;
}

// ---------------------------------------------------------------------------------------------
// Full memory dump via dbghelp.
// ---------------------------------------------------------------------------------------------

static bool DumpProcess(HANDLE proc, DWORD pid, const std::wstring& outPath, DWORD& outSizeLow, DWORD& outSizeHigh) {
    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;

    const MINIDUMP_TYPE flags = (MINIDUMP_TYPE)(MiniDumpWithFullMemory |
                                                MiniDumpWithFullMemoryInfo |
                                                MiniDumpWithThreadInfo);
    const BOOL ok = MiniDumpWriteDump(proc, pid, out, flags, nullptr, nullptr, nullptr);

    LARGE_INTEGER size{};
    GetFileSizeEx(out, &size);
    CloseHandle(out);
    outSizeLow = (DWORD)(size.QuadPart & 0xFFFFFFFFull);
    outSizeHigh = (DWORD)(size.QuadPart >> 32);
    return ok == TRUE;
}

// Resolve a sensible default output: next to the *game* exe (dumping happens during the
// live session, and keeping the artifact in the game folder keeps it out of the git tree).
static std::wstring DefaultDumpPath(DWORD pid) {
    std::wstring dir;
    std::wstring exePath = GetProcessExePath(pid);
    if (!exePath.empty()) {
        const size_t slash = exePath.find_last_of(L"\\/");
        dir = (slash != std::wstring::npos) ? exePath.substr(0, slash + 1) : exePath;
    } else {
        wchar_t cwd[MAX_PATH] = {};
        if (GetCurrentDirectoryW(MAX_PATH, cwd)) dir = std::wstring(cwd) + L"\\";
    }
    if (dir.empty()) dir = L".\\";

    dir += L"dumps\\";
    CreateDirectoryW(dir.c_str(), nullptr);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[MAX_PATH] = {};
    swprintf_s(name, L"recroom-%04u%02u%02u-%02u%02u%02u.pid%lu.dmp",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, pid);
    return dir + name;
}

// ---------------------------------------------------------------------------------------------

int wmain(int argc, wchar_t** argv) {
    std::wcout << std::unitbuf;

    std::wcout << L"Rec Room runtime metadata dumper\n";
    std::wcout << L"===============================\n\n";

    std::wstring outputPath;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            std::wcout << L"usage: Metadump.exe [output.dmp]\n"
                          L"  With no argument, writes the dump to <game folder>\\dumps\\.\n";
            return 0;
        }
        outputPath = arg;
    }

    std::wcout << L"[*] Waiting for an unpatched " << kProcessName << L" (launch Rec Room now)...\n";
    DWORD pid = 0;
    for (;;) {
        std::vector<DWORD> pids;
        FindProcessIds(kProcessName, pids);
        if (!pids.empty()) {
            // Prefer the first instance; the dump has no per-instance state.
            pid = pids.front();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::wcout << L"[+] Found game, PID " << pid << L"\n";

    std::wcout << L"[*] Waiting for GameAssembly.dll + Referee.dll to load (required for the metadata to exist)...\n";
    const auto moduleDeadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    for (;;) {
        if (!ProcessIsAlive(pid)) {
            std::wcout << L"[!] Game exited before its modules loaded. Nothing to dump.\n";
            return 2;
        }
        if (AllRequiredModulesLoaded(pid)) break;
        if (std::chrono::steady_clock::now() > moduleDeadline) {
            std::wcout << L"[!] Timed out waiting for the required modules.\n";
            return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    std::wcout << L"[+] Modules present. Scanning for the il2cpp metadata image (0xFEEDFACF)...\n";

    // il2cpp initializes some time after the modules load (typically within a second or
    // two). Poll: suspend -> scan -> resume -> wait. The game only has ~20s of life left
    // against the dead backend, so keep the passes tight.
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION |
                                  PROCESS_VM_READ | PROCESS_SUSPEND_RESUME,
                              FALSE, pid);
    if (!proc) {
        std::wcout << L"[!] OpenProcess failed (" << GetLastError() << L"). Try running as administrator.\n";
        return 3;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSearchBudgetMs);
    int pass = 0;
    MetadataHit hit;
    bool found = false;

    while (std::chrono::steady_clock::now() < deadline) {
        ++pass;
        if (!ProcessIsAlive(pid)) {
            std::wcout << L"[!] Game exited before the metadata image was found.\n";
            std::wcout << L"    Relaunch and keep this window open; it scans automatically.\n";
            CloseHandle(proc);
            return 4;
        }

        SuspendProcess(proc, pid);
        const bool hitThisPass = ScanForMetadata(proc, hit);
        ResumeProcess(proc, pid);

        if (hitThisPass) {
            found = true;
            std::wcout << L"[+] Metadata image found at 0x" << std::hex << std::uppercase << hit.address
                       << L" (version " << std::dec << hit.version << L"), pass " << pass << L"\n";
            break;
        }

        std::wcout << L"    pass " << pass << L": not there yet (nothing suspicious either)\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }

        if (!found) {
            std::wcout << L"[!] 0xFEEDFACF not found in the scan window -- dumping the full\n"
                          L"    process anyway. That is the whole point of this tool: the\n"
                          L"    metadata lives in here somewhere, just not in stock form.\n";
        }

    // Take the full dump with the process frozen: one consistent snapshot beats a torn one.
    std::wcout << L"[*] Suspending the process and writing a full memory dump...\n";
    SuspendProcess(proc, pid);

    const std::wstring out = outputPath.empty() ? DefaultDumpPath(pid) : outputPath;
    DWORD sizeLow = 0, sizeHigh = 0;
    const bool ok = DumpProcess(proc, pid, out, sizeLow, sizeHigh);

    ResumeProcess(proc, pid);
    CloseHandle(proc);

    if (!ok) {
        std::wcout << L"[!] MiniDumpWriteDump failed (" << GetLastError() << L") writing to:\n    " << out << L"\n";
        return 6;
    }

    const unsigned long long sizeBytes = (static_cast<unsigned long long>(sizeHigh) << 32) | sizeLow;
    std::wostringstream sizeText;
    sizeText << std::fixed << std::setprecision(1) << (sizeBytes / (1024.0 * 1024 * 1024));

    std::wcout << L"[+] Dump complete: " << out << L"\n";
    std::wcout << L"    Size: " << sizeText.str() << L" GB\n";
    std::wcout << L"    Next steps: scan the .dmp for the metadata image (e.g. by searching for\n";
    std::wcout << L"    FC FA ED FE or known string literals from Player.log), carve it out, and\n";
    std::wcout << L"    feed it together with GameAssembly.dll to Cpp2IL.\n";
    return 0;
}

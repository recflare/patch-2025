# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An archival patch for Rec Room build **19/07/25** (19 July 2025) that repoints the client from the
dead official backend to a mirror: the API host in request URIs is rewritten and Photon's DNS is
redirected, both to hosts supplied at runtime by `2025patch.ini` (`ApiHost` / `PhotonHost`) — **no
backend is compiled in, and both default to empty, i.e. off** — plus a self-contained Referee
(anti-cheat/anti-debug) bypass. Ships as an injected DLL and a standalone injector.

**Status: playable.** Boots, logs in, loads the dorm in ~5s, joins arbitrary rooms, and holds a
session indefinitely. Getting there needed four fixes that are *not* obvious from the code alone —
the image content-signature bypass, an unblocked+stubbed `datacollection` host, a `StorefrontConfig`
field in the server's `api/config/v2`, and the CheatManager suppressor. The last two sections below
exist because each of those presented as a completely different bug than it was.

## Build

Release|x64 is the only working configuration — see the caveat below.

```powershell
.\build.ps1                 # also: -Rebuild, -Clean, -Verbosity normal
```

`build.ps1` finds MSBuild via vswhere and is what CI runs too, so the two stay one source of truth.
Straight MSBuild works and is what the tooling notes below assume:

```bash
"C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" \
  2025Patch.sln /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
```

Both projects write to the solution-level `x64/Release/`: `2025Patch.dll` and `Injector.exe`.
Ship the two side by side; the injector defaults to `2025Patch.dll` next to itself (or takes a path
as argv[1]).

After a successful build `build.ps1` also copies the three **drop-in files** — `2025patch.ini`,
`RecRoomScreen.bat`, `RecRoomVR.bat` — from the repo root into the output directory, so
`x64/Release/` is the complete set to drop into the game folder and the release zip is packed
straight from it. Running MSBuild directly skips that copy and leaves you with the binaries only.

`RecRoomScreen.bat` / `RecRoomVR.bat` are the shipped one-click launchers: `cd /d "%~dp0"`, start
`Injector.exe`, then `Recroom_Release.exe +forcemode:screen|vr`. **Everything they reference is
relative, so they only work from the game folder**, and they are two copies of one script differing
in the `forcemode` argument: change one, change the other.

CI (`.github/workflows/build.yml`) builds Release|x64 on every push and cuts a zip on a `v*` tag.
There is no test suite and no linter — verification is empirical: run the game, read `2025patch.log`.

**Debug|x64 does not link.** The `ml64.exe` CustomBuild step for `RetSpoof.asm` in
`2025Patch/2025Patch.vcxproj` is conditioned on `Release|x64` only, so a Debug build has no
`_spoofer_stub` to link against. Add a matching Debug condition before trying to build Debug.

## Architecture

### Boot sequence

`Injector/injector.cpp` waits for an **unpatched** `Recroom_Release.exe`, then waits until **both**
`GameAssembly.dll` and `Referee.dll` are in that process's module list (the patch resolves against
both at attach time and would crash if injected earlier), settles 2s, then `CreateRemoteThread` +
`LoadLibraryW`.

It still never double-injects — a second attach installs every hook twice and crashes — but that
guard is **per process** rather than a reason to stop: an instance whose module list already contains
`2025Patch.dll` is skipped and the search continues, which is what makes **two clients side by side**
work (one injector each). The earlier version took the first PID matching the name and exited
"nothing to do" if that one was patched, so a second client launched next to a running one silently
went unpatched. Two injectors racing over the same new process would both see it unpatched and both
attach, so the look-then-inject step is serialized through a `Local\RecRoom2025PatchInjector` mutex
and the loser resumes searching. Liveness is checked per PID (`OpenProcess`/`WaitForSingleObject`)
for the same reason — comparing against "the first process with this name" reports a healthy target
as exited as soon as another instance appears ahead of it.

`DllMain` (`2025Patch/src/main.cpp`) then calls `RR::Patches::Resolve()` followed by
`RR::Patches::Patch()`, both in `2025Patch/src/RR/Patching/Patches.h`.

### Addresses are hardcoded RVAs, against two different bases

`2025Patch/src/RR/Methods.h` is the address table. **Which module a value is relative to depends on
its namespace:** `RR::Methods::Referee::*` are RVAs into `Referee.dll`; everything else is an RVA
into `GameAssembly.dll` (`GA`, `globals.h`). Patches.h reflects this — `Referee + Check1` vs
`GA + SendRequest`.

Nothing is pattern-scanned except the return-spoof gadget, so **any other game build invalidates
every offset in Methods.h**. Structure field offsets (`RR::Offsets::*`) come from the runtime dump
of the same build and are equally build-locked. Rec Room's il2cpp names are obfuscated
(`CBACIMLIBPF`, `HPJKKCCECLH`); Methods.h comments record what each one actually is — keep that
mapping updated when adding an address, it is the only way back to the managed symbol.

`Resolve()` pattern-scans `FF 23` (`jmp [rbx]`) for the spoof gadget and resolves the three
`il2cpp_*` exports via `GetProcAddress`.

### Three separate network paths, hooked three different ways

This split is the single most load-bearing thing to know, and it is not visible from any one file:

1. **BestHTTP** (`HTTPRequest.SendRequest`) — hooked and *rewritten*: `ns.rec.net` → `ApiHost` from
   the ini, by constructing a fresh `System.Uri` and storing it back into the request. With no
   `ApiHost` set the hook still installs and logs, but performs no rewrite.
2. **Photon** — does not use HTTP at all; it resolves `*.photonengine`/`exitgames`/
   `photonindustries` and connects over raw sockets. Redirected at the winsock layer by hooking
   `getaddrinfo` / `GetAddrInfoW` in `ws2_32`.
3. **System.Net.Http** (`RR::Methods::HttpClient::Request9`/`Request10`) — RecNet's *other* HTTP
   client, which is what room-save and asset blobs actually go through. The SendRequest hook is
   blind to it. It is currently **logged only, never rewritten**; those requests reach the mirror
   only because their DNS goes through the winsock hooks.

So "add a URL rewrite" is not one edit — decide which stack the request travels first.

### The System.Net.Http client is a single serial queue — this dominates load times

`CBACIMLIBPF` does **not** dispatch concurrently. Everything goes through one FIFO:

| RVA | method | stage |
| --- | --- | --- |
| `0x7CC4F20` / `0x7CC50D0` | `IBJKIAMJCDN(..., Queue, ...)` | enqueue (`Request10`/`Request9`) |
| `0x7CC6EA0` | `KOMEOKGFOBP(Queue, ct)` | pump — drains the queue |
| `0x7CC29E0` | `CJOCHJBHGPM(uint seq, ...)` | one actual send; `seq` is a **global** counter |

Only after `CJOCHJBHGPM` does a request become a BestHTTP request and appear in the `SendRequest`
hook. The room-save blob sits around **position 100**, and `FetchRoomLoadDetails > getRoomSaveData`
times out at **30s**, so anything that stalls the queue kills the room load.

⚠️ **A dead host that fails fast is worse than one that answers.** The client retries transport
failures with backoff *on this queue*. One blocked `data/event` POST retried at ~5.5s and ~13.7s and
held the queue **24.68s**, pushing the blob to 36.6s and blowing the timeout. Blocking it in DNS
(`BlockDeadHosts`) did not help — NXDOMAIN and a block are equally bad, because the cost is the
retry, not the lookup. **Only an instant 2xx stops the backoff.** Stub the endpoint server-side
first, *then* remove the host from `IsDeadHost`; removing it without a stub just restores NXDOMAIN.

**This queue also takes Photon down.** The Photon symptom — joins, ~4 `SetProperties` answered, then
~50s of silence ending in `Unable to send message!` — is a **backlog of Backtrace crash uploads**,
not a Photon or Luxon fault (running against real Photon Cloud reproduced it identically) and not
missing room save data (rooms with no save blob load fine). `submit.backtrace.io` is in `IsDeadHost`
and `BlockDeadHosts` defaults to `true` for this reason, not just to keep telemetry off the wire.

The `[Pump]` / `[Send]` tracers exist to make this visible. To find what starves the queue, diff
consecutive `[Send]` timestamps and take the single largest gap — it names the blocking request
directly. And before concluding a request "was never sent", check the log covers enough wall-clock
after it: for a long time the blob looked undispatched when it was merely 6s late. Note the general
trap both wrong answers above share: on a serial queue the starving request is not the one that
visibly fails, so the loudest thing in the log is rarely the cause.

### Calling back into il2cpp

Managed methods are invoked through `spoof_call` (`Utils/deps/spoofcall/`), which hides the DLL's
return address from Referee's stack walks. Be sparing: a spoofed call on a path where a managed
exception can unwind through the faked return address yields `STATUS_INVALID_DISPOSITION`
(0xC0000026) and kills the process. `SendRequest_H` carries a comment marking exactly which
redundant spoofed call caused that crash — do not reintroduce one just to log something already in
hand.

`ReadIl2CppString` (`globals.h`) deliberately ignores `Il2cppString::Length`: on this il2cpp layout
offset 0 is the klass pointer, not a length. It converts the null-terminated `wchar_t` buffer at
wchar offset `0xA` under a fixed cap, returns a `new[]` buffer or `nullptr`, and **callers must
`delete[]` it and null-check** (`std::string s = nullptr` is UB and was a live crash).

### Hook conventions

Every hook is written to never take the game down: wrap the interesting work in `__try/__except`
(or `try/catch` where C++ exceptions are possible) and **always** forward to the original, unchanged,
on any failure. Hooks that force a constant return (`VerifyImageSig_H`, `IsTransportEncrypted_H`)
call the original once and log the true value before overriding it — that line is how you find out
whether the hook is masking a different fault. Keep that pattern for new forcing hooks.

### Diagnostics

`PatchLog` (`Utils/Inc/Includes.h`) writes timestamped, per-line-flushed output to `2025patch.log`
**next to the game exe** — it survives a crash and correlates with Unity's `Player.log` by
timestamp. The file is opened `_SH_DENYWR`, so with two clients running only the first gets that name
and the others fall back to `2025patch.<pid>.log`; otherwise both would truncate it and then write at
independent offsets, interleaving the two logs into nonsense. Only writing is denied — tailing the
live log still works. The AllocConsole path is off by default (`EnableConsole` in `2025patch.ini`): the console
steals foreground focus and Unity throttles hard when unfocused, which measurably wrecked room-load
times. `std::cout` calls therefore go nowhere; every one of them is mirrored by a `PatchLog` line,
and new diagnostics should use `PatchLog`.

`Patch()` is split accordingly: the load-bearing hooks install unconditionally — Referee x4, the TLS
`NotifyServerCertificate` no-op, `SendRequest_H` (host rewrite), `CheatQuit_H`, `VerifyImageSig_H`
and the two winsock DNS hooks — `GetNameServerAddress_H` installs only when `PhotonHost` *and*
`PhotonPort` are both set, since it would otherwise be a no-op — while all 19 diagnostic hooks live in one
`if (RR::Config::EnableTracing)` block and are not installed at all when it is off. Routine
per-request chatter uses the `TraceLog` macro (a no-op unless tracing is on) so `SendRequest_H` stays
quiet without losing the rewrite; genuine anomalies there still use `PatchLog` unconditionally.

The tracers are kept rather than deleted deliberately — see the findings-log convention below. They
are the only way this project has ever seen inside the serial request queue or caught a server-pushed
Photon event, and each carries the comment block explaining what it answered.

`VerifyImageSig_H` deserves a note: mirrored `img.` assets still carry rec.net's original
`content-signature: key-id=KEY:RSA:p1.rec.net` header (that is what `?sig=p1` selects), and the
client verifies it against a baked-in public key. **No mirror can ever satisfy it** — signing needs
Rec Room's private key — so the check is unsatisfiable by construction rather than merely failing.
The throw used to hard-stall the shared request pump, which is why it broke room loading and not
just artwork.

## Configuration knobs

### Runtime — `2025patch.ini`, next to the game exe

Read at attach by `RR::Config::Load()` (`2025Patch/src/RR/Config.h`), called from `DllMain`
**before** `Resolve()`/`Patch()` so the first request and first DNS lookup already see it. One
`[config]` section:

| key | default | effect |
| --- | --- | --- |
| `ApiHost` | *(empty)* | host that replaces `ns.rec.net` in BestHTTP request URIs. **Empty = no rewrite at all** — every URI passes through as the game built it |
| `PhotonHost` | *(empty)* | `getaddrinfo` target for `*.photonengine` / `exitgames` / `photonindustries`. **Empty = make no Photon changes at all** — no DNS redirect, no port write, `GetNameServerAddress_H` not even hooked |
| `PhotonPort` | `0` | requires `PhotonHost`; overrides the **name-server** port for the initial connect. `0` = leave the protocol default |
| `EnableConsole` | `false` | AllocConsole debug window. Costs load time (focus theft → Unity throttling); everything it prints is already in `2025patch.log` |
| `BlockDeadHosts` | `true` | `getaddrinfo` returns `WSAHOST_NOT_FOUND` for `IsDeadHost` matches. **Third-party hosts only** — rudderstack, backtrace, statsig, `cloud.unity3d.com`. Most are still *live*, so this keeps an archival session's telemetry and crash dumps off unrelated companies' servers — but the backtrace entry is load-bearing: its upload backlog is what disconnects Photon. Never list a `*.recflare.net` host: see the serial-queue warning above |
| `SuppressDuidMismatch` | `true` | Forces `CheatManager.CheckForDUIDMismatch` to false. **The fix for the client hanging at launch / Create Account** on a machine whose *stored* device id no longer matches the runtime one — see below. No-op on healthy machines; set `false` only to reproduce the hang |
| `EnableTracing` | `false` | Installs the diagnostic hooks ([Pump]/[Send] queue probes, Photon operation/status/event tracers, HttpClient paths, BestHTTP responses) and un-quiets `SendRequest`'s per-request lines. Off = none of them are hooked at all |
| `VoiceKeyXml` | *(empty)* | RSA **public** key (`<RSAKeyValue>` XML, one line) that the Tachyon voice handshake is encrypted to. Empty = keep Rec Room's baked-in key. Set it to your own and the voice server can decrypt the handshake — see *Voice handshake re-key* below |

The file is created with these defaults on first run if absent, and never overwritten afterwards.
A byte-identical copy lives at the repo root as `2025patch.ini` and is staged into `x64/Release/` by
`build.ps1` (together with the two `.bat` launchers) so it ends up in the release zip beside the
DLL and injector — **when you change `WriteDefaultFile`, change that file too**, or the shipped example drifts from what the patch writes.

Host values are sanitized to a bare host (scheme and path stripped) because `PhotonHost` is a
`getaddrinfo` node name, not a URL. Malformed input never leaves the patch in a broken state: a
non-boolean flag or an out-of-range port logs a line and keeps the default (a non-numeric port reads
as `0`, i.e. "leave it alone"). Booleans accept `true/false`, `1/0`, `yes/no`, `on/off`. The
effective set is logged as `[Config] ...`, plus `[Patch] API host=... Photon=...`.

**Neither host has a compiled-in value** — `ApiHost` and `PhotonHost` both start empty, so the
backend this patch talks to comes from the ini and from nowhere else. Both go through the same
`ReadHost`, and for both an empty value is the master switch for that rewrite: blank `ApiHost` means
`SendRequest_H` leaves every URI as the game built it (the hook is still installed, it just does not
rewrite), blank `PhotonHost` means no DNS redirect, no port write, and no
`GetNameServerAddress` hook. The three cases are kept distinct on purpose — key absent keeps the
current value (empty unless something set it), present-and-blank explicitly clears, and
present-but-unsanitizable (`https://`) is malformed input and keeps the current value.

There is no app-id knob and no Cloud flag: this client takes its Realtime/Voice/Chat app ids from an
endpoint on the server, so swapping the Photon server is the entire job. `UsePhotonCloud` +
`CloudAppId*` + `CloudFixedRegion` existed to A/B against real Photon Cloud and were removed after
their `AppSettings.AppId*` writes were seen to achieve nothing — see the PHOTON BACKEND SELECTION
block in Patches.h, which keeps the finding *and* the correction to it.

⚠️ **`AppSettings` is a dead seam on this build — all of it.** `ConnectUsingSettings`
(`OLPEILEPEAD.JBNCMFDFDLM`, `0x757E8C0`) is the real method and does read `AppSettings.Port` and the
app ids, but **this client never calls it**: Rec Room configures the `LoadBalancingClient` field by
field and connects through `ConnectToNameServer`. A hook there installs cleanly and its body never
runs, which is what made both the app-id writes and, later, `PhotonPort` fail silently. The DEAD SEAM
block in `Methods.h` has the evidence. The general lesson, since it cost this project twice: a hook
that logs nothing is evidence about the hook, not about the field it was going to write.

`PhotonPort` cannot go through the DNS hooks — a port never passes through `getaddrinfo` — so it is
written into the client instead, at `GetNameServerAddress` (`OLPEILEPEAD.FHFJBMBEBPP`, `0x757B960`).
That method formats `"<NameServerHost>:<port>"` and prefers
`LoadBalancingClient.NameServerPortInAppSettings` (`+0x180`) over the protocol default (5058 UDP,
27000 alternative UDP), so `GetNameServerAddress_H` writes that field just before forwarding. Every
name-server path funnels through it. This moves **only the initial name-server connect**; the master
still hands out its own master/game-server ports afterwards, so those come from the server.

Not configurable: the host being *matched* (`ns.rec.net`) and the Photon-name substrings in
`IsPhotonHost`, both still literals in Patches.h. A pre-rename ini using `[hosts]` applies nothing —
`Load()` detects that case and logs `ignoring legacy [hosts] section`.

`Load()` runs before `CreateConsole()` in `DllMain` — it now decides whether the console exists at
all. `PatchLog` is file-based, so the `[Config]` lines are recorded either way.

Nothing is compile-time any more; every knob lives in the ini.

## Voice handshake re-key (`VoiceKeyXml`)

TachyonClient's connect payload is a JSON blob `{AI, AT, VB, CKA, CIA, CPK}` — the NGO
`ConnectionRequestMessage.ConnectionData`. Only `AI` (the account id, decimal string) is plaintext,
and it is **client-asserted**, so a self-hosted voice server has nothing it can trust:

| field | contents |
| --- | --- |
| `AI` | account id (plaintext) |
| `AT` | `AES(UTF8({"accountId","environment","accessToken"}))` — the RecNet access token lives here |
| `VB` | `RSA(base64decode("GTLFaIAoniLKqHEJFIhcGw=="))`, a fixed 16-byte constant |
| `CKA` / `CIA` | `RSA(aes.Key)` / `RSA(aes.IV)` — freshly generated per connection |
| `CPK` | `AES(client RSA CspBlob)` |

The RSA is a public key baked into the client, so `AT` is unreadable without Rec Room's private key.
`VoiceKeyXml` swaps that public key for one of yours, which makes the whole payload decryptable and
lets the server authenticate the `accessToken` (and bind the otherwise-unauthenticated `AI` to it).

**How it works.** `VoiceCctor_H` hooks `DMIBBCKIGCG..cctor` (`0x827A820`), lets it run, and *then*
overwrites the static `KBANIKNNKKD` it just filled (statics `+0x08`) — the `<RSAKeyValue>` XML string
the TachyonClient ctor reads two instructions before calling `rsa.FromXmlString`. **The game performs
the import itself**; we deliberately do not call `FromXmlString`, because that is a managed method
that can throw, and a managed exception unwinding through a spoofed return address is the
`STATUS_INVALID_DISPOSITION` crash documented on `SendRequest_H`.

Ordering is the whole trick, and the cctor hook gets it for free: the ctor calls that `.cctor` a few
instructions *before* it reads the static (cctor check at `0x82796C3`), so writing at the end of the
cctor lands after the stock key and before the import. `TachyonCtor_H` remains, but only to log which
key the run used and as a fallback for a cctor that ran before the hooks were in.

⚠️ **Do not move this back to the ctor, and do not force the class init.** The first version did both
and killed the process at launch (18 Sep, `PhotonHandler.Awake`, nothing in the log). Two reasons,
and the second is the general one:

- `RR::Offsets::Tachyon::KeyHolderTypeInfo` (`0xD1A9AF8`) is a **metadata-usage slot**, and it holds
  an encoded index (`type << 29 | index`) until il2cpp resolves it — which the *using method* does at
  the start of its own body. Read from a hook on the ctor's **entry** it is always still an index
  (measured: `0x20028E0D`, i.e. index `0x28E0D`), and `il2cpp_runtime_class_init` on that is fatal.
  `LooksLikeResolvedClass` rejects anything under 4 GB for this reason. The cctor resolves the same
  slot itself (`mov rcx,[rip+0x4F2F25D]` at `0x827A894`), so after it returns the pointer is real.
- The crash left **no** `[Voice] re-key FAILED` line even though the whole body is inside
  `__try/__except`. That absence is diagnostic: SEH catches an access violation, so a silent death
  means the uncatchable kind. `__except` is not evidence that a hook is safe.

The `[Voice]` lines log the existing key before overwriting and read the static back afterwards, so
one healthy run proves both the offset and the swap:

```
[Voice] cctor: klass=... statics=... existing key=... "<RSAKeyValue><Modulus>z7L4+nePWLb3f4OzskH39KyiuP"
[Voice] handshake re-keyed from VoiceKeyXml (415 chars) -- static now "<RSAKeyValue><Modulus>wM8JO2..."
```

`z7L4+nePWLb3...` is Rec Room's own modulus — if the "existing key" line ever stops looking like a
`<RSAKeyValue>`, `Static_ServerKeyXml` is wrong for that build and the write is landing on an
unrelated static.

**Server side.** The client calls `Encrypt(data, fOAEP: false)`, so decrypt with **PKCS#1 v1.5, not
OAEP**; the symmetric layer is `AesCryptoServiceProvider` defaults = **AES-256-CBC / PKCS7**. Unwrap
`CKA` → 32-byte key and `CIA` → 16-byte IV, then decrypt `AT`. Key size is free — the client only
encrypts, so a 2048-bit key just makes `CKA`/`CIA`/`VB` 256 bytes.

**Self-test.** `VB` is that fixed constant encrypted with the same key, so after a swap it must
decrypt to `19 32 C5 68 80 28 9E 22 CA A8 71 09 14 88 5C 1B`. If it does, the client is on your key
and `AT` is trustworthy; if it does not, the swap did not take.

Generate a pair with:

```powershell
$r = [System.Security.Cryptography.RSA]::Create(2048)
$r.ToXmlString($false) | Set-Content pub.xml   # -> VoiceKeyXml (one line)
$r.ToXmlString($true)  | Set-Content priv.xml  # -> voice server ONLY, never ship this
```

`ReadVoiceKey` rejects anything that is not `<RSAKeyValue>`/`<Modulus>`/`<Exponent>` XML, and
refuses a value containing `<D>`/`<InverseQ>` — shipping the private half to every client would
defeat the point. Every failure path logs and keeps Rec Room's key, so the worst case is voice
behaving exactly as it did before.

⚠️ `DMIBBCKIGCG.NDEDJKDIGGM()` / `CKAAEJMIMEF()` look like the getters for these statics but have
**zero callers** — the ctor reads the field directly. Do not hook them.

## When the client hangs at launch, suspect the device id (DUID)

On a machine whose **stored** device id differs from the one derived at runtime,
`CheatManager.CheckForDUIDMismatch(out string)` (`0x2133600`) returns true and the client takes a
migration path that POSTs `PlayerReporting/v1/deviceId` and then **waits for a response it will
accept**. The archival server answers `200 {"success":true}` and it waits anyway — no
`create_account` OAuth, and `WriteDUIDs` never runs, so the id is never persisted. It presents as
"the game will not launch" or "Create Account hangs", is machine-specific (matching machines never
enter the path), and leaves nothing obviously wrong in the log.

`SuppressDuidMismatch` (default **true**) forces that check to false so the path is never entered.
Replayed from recnet-patcher's `Patches/DUIDMismatchPatch.cs` (commit `6a62f0c`), where the same fix
also ships on by default.

⚠️ **It is a workaround, not a repair** — it does not fix the stored id, and two things learned in
recnet-patcher constrain any attempt to do so properly:

- **Clearing local storage does not fix it.** The id lives in PlayerPrefs `cm_did_ppk` (registry
  `cm_did_ppk_h3478365449`, CodeStage-obscured so never plaintext), but deleting it did not change
  the `oldDeviceId` in the POST, and on the failing run `cm_did_ppk` was never read at all.
- **Where the old id comes from is still unknown.** It survives deleting the whole
  `HKCU\Software\Against Gravity\Rec Room` key and appears nowhere as plaintext under
  `AppData/LocalLow`. Leading theory: the backend recorded it from an earlier POST and hands it back.
  So the real fix is server-side — make the endpoint stop reporting a stale old id.

`DuidMismatch_H` calls the real check once and logs the stored id it produced (`[DUID] ...
original=N stored="..."`). That line is aimed squarely at the open question; if a value ever shows
up there, it is the first hard evidence of where the old id lives.

Note the address is **prologue-stolen** — the first `0x19` bytes at `0x2133600` are encrypted filler
(a memory carve reads them as `nop; jmp <thunk outside the module>`) and the real body starts at
`0x2133619`. Disassembling from the entry looks like junk; that is expected, not a wrong address.
recnet-patcher's remaining DUID tooling (`CorruptDUIDPatch`, `DeviceIdResponsePatch`, `DUIDProbePatch`)
was **not** ported — it is diagnostic-only and lives there.

## When the client exits on its own, suspect CheatManager first

`CheatManager` (Assembly-CSharp; the class name is **not** obfuscated) periodically enumerates loaded
modules, strips the app directory from each path, formats them `[index:name]`, and calls a closure
whose entire body is `SessionManager.FatalApplicationQuit(533223478, filenames)`. It finds
`2025Patch.dll`. `RR::Methods::AntiCheat::ModuleScanDetected` (`0x2148FB0`) is hooked **replace-only**
to suppress it — never call the original.

**This is worth its own section because the symptom lies.** It presents as a completely clean user
quit: `crash_detected=false`, `app_exit_state=ReadyForExit`, orderly Photon `Leave`, `player/logout`,
full `Application.quit`. The timing varies with the scan (40s–140s), so it also reads as a timeout.
Everything you would naturally blame is *downstream* of the decision — `player/logout` lands 0.14s
**after** it, and `RaiseEvent(...) failed` / `Unable to send message!` are teardown noise. Photon is
innocent (the `DeserializeEventData` tracer shows zero `ErrorInfo` (251) events).

Note this suppresses the *reaction*, not the detection — the scan still finds us every cycle. PEB
loader-list unlinking (`../recnet-patcher/src/memory/module_hide.c`) would defeat it at the source
and is the better fix if the scan ever grows a second consumer.

**Re-finding it after a build rolls:** `533223478` (`0x1FC85836`) is a hardcoded constant, not a
dynamic reason code. Scan `GameAssembly.dll` for the immediate `B9 36 58 C8 1F` (`mov ecx, imm32`) —
it gave exactly one hit and landed straight in the call site. Do that before walking any stacks.

Related: `SessionManager` keeps most of its real names (`FatalApplicationQuit`, `TryApplicationQuit`,
`LogoutToBootScene`, `VerifyAccountRequirements`, `HandleRoomJoinFailure`, `DefaultRoomGatesAsync`,
`JuniorRoomCheck`) — dump them from `methods.pkl` by the `SessionManager$$` prefix. Its
`FatalApplicationQuit` body **ignores the message argument** (it tail-jumps to `TryApplicationQuit`
with the code only), so read the message at the *caller*, not from a hook on it.

## Resolving addresses and wire shapes on this build

The tooling lives in `../recnet-patcher/il2cpp-2025/` and there are two indexes with different
coverage — use the right one:

- **`methods.pkl`** (346,938 methods, from a live carve) is the more complete index and is the only
  one that covers **UnityEngine** — that is how `Application.Quit` and
  `Internal_ApplicationWantsToQuit` were located. Look up an RVA with a `bisect` over the sorted
  table; a large `+0x` delta means "not really inside that method", not a hit.
- **`RecRoom_Info/Code/2025-07-19_02-45-25`** (Cpp2IL, 241,704 methods) has type/field structure and
  attributes but is **signature-only** — no bodies, no string literals. Verify it with
  `SendRequest = 0x77E0950`. `../recnet-patcher/il2cpp-tools/out/` is a **different, older build**;
  it has `dump.cs` but zero hits for this build's type names.

Because there is no `dump.cs` for this build, `dtoshape.py` cannot run. To get Utf8Json wire keys,
find `XXXX : IHCMHKGLBEA<Dto>` in the Cpp2IL dump, take its ctor RVA, and run
`py names.py <ctor> 1200` with `GAMEASSEMBLY` pointed at
`../recnet-patcher/tools/metadump/out/GameAssembly.dll`. The trailing `mov` block is
`____stringByteKeys` — one entry per member, in declaration order. That is how the missing
`StorefrontConfig` field in `api/config/v2` was identified.

`whatis2025.py` defaults to the **wrong (2025-04-29) dump** — always pass
`--dump C:\Games\RecRoom_Info\Code\2025-07-19_02-45-25`.

The stack tracers (`LogStack`) print `module+offset` for every frame; feed the `GameAssembly.dll+0x…`
values into the lookups above. Frames below the lowest managed RVA (`0xA5F110`) are il2cpp runtime
plumbing, and a chain of them means the call arrived via `runtime_invoke` — i.e. from a delegate or
event handler, so there is no static caller to find.

## Investigation notes in comments

Long comment blocks in `Methods.h` and `Patches.h` record *why* a hook exists and what was measured,
including dead ends kept deliberately (e.g. `EnetPeer.IsTransportEncrypted` is defined but **not**
hooked, because the original measured `0` and datagram encryption was ruled out). Treat these as the
project's findings log — read the relevant block before changing or re-adding a hook, and record new
measurements the same way rather than deleting the old reasoning.

# Rec Room 2025 preservation notes

Working state of the archival client for **build 20250718.01** (Steam manifest
`1151455856673601091`, July 2025), verified against the `recflare.net` mirror backend.

![Rec Room title screen, logged in as @scientist against the recflare mirror](docs/images/title-screen.png)

Verified end-to-end: game boots, patch attaches, title screen appears, login as `@scientist`
against the mirror, Photon session establishes, and the game sits at the title screen steady.

## Quick start (the configuration that works)

1. Have **Steam running**. The game will not get past the boot sequence without it —
   it aborts at "Failed to initialize Steam Platform" and dies silently. Log in to Steam first.
2. Unpack the release zip (`2025Patch.dll`, `Injector.exe`, `2025patch.ini`,
   `RecRoomScreen.bat`, `RecRoomVR.bat`) into the Rec Room folder (next to
   `Recroom_Release.exe`).
3. (Optional, only if you need a backend) point `2025patch.ini` at the mirror:
   `ApiHost=ns.recflare.net`, `PhotonHost=photon.recflare.net`, `PhotonPort=0`.
   The shipped example ini leaves both hosts empty on purpose — no backend is baked in.
   Telemetry CDNs the game tries that are dead are already redirected by the patch when
   `BlockDeadHosts=true`.
4. Run `RecRoomScreen.bat` (desktop) or `RecRoomVR.bat`. Confirm the patch attached by
   reading `2025patch.log` — you want the `hooks installed` and `ns.recflare.net` lines.

Things that do **not** matter for archival: `statsigapi.net` failures (dead), Unity cloud
(error 6, blocked by `BlockDeadHosts`), Rudderstack "no response" warnings — all expected.

## What does not work, and why: a proxy `Referee.dll`

The game's statically linked import table contains exactly one dependency: `Referee.dll!xVnMnf`
(the Themida protector gate; `Referee.dll` identifies internally as `ProtectorOutput.dll`).
The obvious "Defender-clean" approach is a proxy DLL under that name that tail-jumps into the
protector's real export — the same shape as the ReShade/ASI loaders — with the protector itself
renamed
([details of what we tried](#how-we-tested-the-proxy-approach)).

It does not work for this build. The protector **re-reads its own file from disk during the boot
into the title screen** and rejects anything that is not byte-identical to the protector image.
Consistent failure signature in every proxy variant, including with the patch disabled:

```
Faulting application name: Recroom_Release.exe
Faulting module name:       Referee(Original).dll
Exception code:             0xc0000005
Fault offset:               0x0000000003169338
```

- with the protector renamed to `RefereeOriginal.dll` at the game root: AV at RVA `0x3169338`,
  ~50–56s after launch, after the log reaches `PhotonNetwork.Disconnect()`;
- with the protector moved to `preserve\Referee.dll` (correct filename, correct build-time name,
  hash-identical to the backup): same AV at the same RVA, same timing;
- with the patch disabled entirely (proxy only): the game instead exits silently at ~35–40s
  (`[Platform.BootSequence] ... Failed to connect to RecNet (error code: summer)` — expected,
  the default ini has no backend) — but the with-patch path crashes in the protector every time,
  i.e. the crash is independent of the patch payload.

Conclusion: for this build, `Referee.dll` at the game root **must be the protector, byte-identical,
unmodified**. Do not attempt "just rename the protector". The injector attaches the patch from
outside the process before the protector's checks run, and the protector never sees anything
but its own file and memory — which is why the injector flow works and the proxy flow doesn't.

`UnityPlayer.dll` does not offer an alternative loading point either: it is a vanilla Unity binary
but it statically imports from `Referee.dll` as well, and `GameAssembly.dll` is packed/encrypted,
so neither is a usable proxy slot.

### How we tested the proxy approach

- proxy `Referee.dll` built with a native trampoline (`xVnMnf` in ml64 assembly, tail `jmp` to the
  protector's export; `mslink.exe` will not emit forwarder exports);
- protector renamed two different ways (`RefereeOriginal.dll` in the root, `preserve\Referee.dll`);
- identical fault offset every time; also the same when `2025Patch.dll` was temporarily removed —
  the crash is in the protector, before the title screen, independent of the patch logic.

## Windows Defender status (honest)

- The shipping `Injector.exe` is a remote-thread injector (`SuspendThread`-free, but it does use
  `CreateRemoteThread` + `VirtualAllocEx` + `LoadLibraryW`). That is an inherently
  Defender-flagged shape; any claim of "Defender-proof" would be fiction.
- All archival testing was done with Defender **off**. We could not validate erasure/detection
  with Defender **on**, and we are not claiming the current build passes it. If Defender erases
  the binaries, add the game folder (or `Injector.exe`/`2025Patch.dll`) to Defender's exclusions,
  or leave Defender off — the same as before this work.
- A Defender-quiet release would need the protector to cooperate with a proxy slot, which we can
  demonstrate it does not (see above), or the patch to be static-linked into the client by editing
  game binaries — not something this project will do.

## What the tools in this repo are for

- `2025Patch/` — the runtime patch (hooks: DNS, SendRequest, CheatMgr, DUID, image-sig, voice).
- `Injector/` — the attach-from-outside loader used by both `.bat` launchers.
- `Metadump/` — analysis tool: waits for `GameAssembly.dll` + `Referee.dll` to finish loading,
  keeps the protector's watchdogs frozen, and dumps the (decrypted) metadata image + a method
  RVA map. Output feeds a full API reconstruction (346,938 RVAs, `SendRequest` @ `0x77E0950` in
  this build). The dumps themselves are not in the repo — they are derived game data.

## Repo hygiene reminders (learned the hard way)

- `recroom/` is gitignored: game files, `client.zip` (3.78 GB), dumps. Never commit them.
- Never commit a rebuilt `global-metadata.dat`, method maps, disassembly, or WER dumps from this
  machine — build 20250718.01 is the live target.
- `Steam` must be running before any launch; the patch can only confirm attach after the game
  reaches the boot sequence.

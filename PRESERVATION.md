# Rec Room 2025 preservation notes

Working state of the archival client for **build 20250718.01** (Steam manifest
`1151455856673601091`, July 2025), verified against the `recflare.net` mirror backend,
with **two** supported ways to attach the patch — no injector required for the default one.

![Rec Room title screen, logged in as @scientist](docs/images/title-screen.png)

Verified end-to-end (desktop mode, Steam running): game boots, patch attaches, title screen
appears, login as `@scientist` against the mirror, Photon session establishes, steady at the
menu.

## Layout

The preservation release is a flat drop-in set next to `Recroom_Release.exe`:

| file | role |
| --- | --- |
| `Recroom_Release.exe` | the game (not in the zip — you already have it) |
| `Referee.dll` | the preservation loader after install (before install: the protector) |
| `PreserveLoader.dll` | same loader, in its shipped name; copied to `Referee.dll` by the installer |
| `2025Patch.dll` | the patch itself |
| `Install-PreserveLoader.bat` | moves the protector to `preserve\Referee.dll`, installs the loader, moves the patch into `preserve\modules\` |
| `RecRoomPreserve.bat` | launcher for the loader flow (no injector) |
| `Restore-RefereeProtector.bat` | one-click revert: protector moves back to `Referee.dll`, patch back to the game root |
| `preserve\modules\*.dll` | the mod folder (see below) |
| `Injector.exe`, `RecRoomScreen.bat` | the classic injector flow, unchanged |

## Quick start (loader flow — the default we recommend)

1. Have **Steam running** and logged in. Without it the game dies silently in the boot
   sequence ("Failed to initialize Steam Platform").
2. Copy the full release into the game folder (next to `Recroom_Release.exe`).
3. Run `Install-PreserveLoader.bat` with the game **closed**. It:
   - moves the game's `Referee.dll` (the Themida protector) to `preserve\Referee.dll`;
   - copies `PreserveLoader.dll` to `Referee.dll`;
   - moves `2025Patch.dll` into `preserve\modules\`.
4. Run `RecRoomPreserve.bat`.
5. Verify in the logs:
   - `preserve-loader.log`: `protector resolved ... xVnMnf=...`, then one
     `loaded <name>.dll` line per module;
   - `2025patch.log`: `[Patch] hooks installed: Referee x4, TLS, SendRequest, ...`
     and the `ns.<host>`/`api.<host>` lines.

### The module folder

`preserve\modules\` is the extension point — **every `*.dll` in it is loaded**, in
**alphabetical filename order**, once `GameAssembly.dll` is loaded and stable. That covers
this build's own patch and anything else with a `DllMain` hook, without a rebuild of the
loader. A module that fails to load is logged (`FAILED to load ...`) and skipped; it never
blocks the rest.

- Want to control load order? Number your files: `0000-loader.dll`, `0100-yourmod.dll`,
  `0200-2025Patch.dll`.
- Drop-in only: when you add/remove a module, just restart the game.
- The patch (`2025Patch.dll`) and any extra mods all sit in the same folder; nothing gets
  special-cased by name.

## Quick start (injector flow — the classic)

1. Steam running.
2. Copy the release into the game folder.
3. Run `RecRoomScreen.bat` (desktop) or `RecRoomVR.bat`. It starts `Injector.exe`
   (waits for `Recroom_Release.exe`, `GameAssembly.dll` and `Referee.dll` to finish loading,
   then attaches) and launches the game. Re-running it against a running game is a no-op.

## Why the loader exists: the real story of the proxy attempt

The game's statically linked import table contains exactly one dependency,
`Referee.dll!xVnMnf`. That makes a proxy `Referee.dll` (a module the game binds through
its own import chain, with no remote thread or cross-process writes) the cleanest possible
attachment point.

The first proxy **did** boot the game to the title screen, but it crashed in the boot
phase every time — deterministically, `0xc0000005` at module RVA `0x3169338` in the
protector, around title-screen entry. We initially documented that as "Themida re-reads
its own file and rejects the proxy". That was wrong.

The actual root cause: `2025Patch` resolved the protector base with
`GetModuleHandleA("Referee.dll")`. Under a proxy, that handle is the tiny proxy module —
the four protector-check hooks (`Check1..Check4`) were aimed at `proxy + protector RVA`,
i.e. memory in the wrong module, so the checks were never NOPed and some unrelated region
took the hook damage. The no-patch proxy run exits cleanly at the RecNet dead-end (the
default ini has no backend), which is exactly what that run looks like with an intact
protector.

Fix, in this release:
- `2025Patch` resolves the real protector via a `GetPreserveProtector()` export from the
  loader when one exists (i.e. when it loaded under the proxy), falling back to
  `GetModuleHandleA("Referee.dll")` otherwise — so the classic injector flow is unchanged.
- The four protector-check hooks now log `MH_STATUS` on failure instead of failing silently,
  so a mis-mapped hook is named in `2025patch.log` rather than surfacing as a crash.
- `PreserveLoader` loads modules from `preserve\modules\` in alphabetical order instead of
  a single hard-coded DLL.

The earlier "this cannot work" documents in the repo were written before this fix. Ignore
them — PRESERVATION.md is the only authority and it describes the fixed build.

## Windows Defender status (honest)

- The loader flow creates **no remote threads, no `CreateRemoteThread`, no RWX pages** —
  the game binds the loader through its own import table. That shape is not
  Defender-flagged in the way the injector is.
- `Injector.exe` remains a remote-thread injector (`SuspendThread`-free but uses
  `CreateRemoteThread` + `LoadLibraryW` remotely). It is kept because it works and is the
  default for people who do not want to shuffle files.
- All verification was done with Defender **off**. We could not validate erasure/detection
  with Defender **on**; nothing here is guaranteed against it. If Defender erases the
  binaries, add the game folder to Defender's exclusions.

## Reproducing what this document claims

```
git checkout <the PR branch> && .\build.ps1          # Release|x64 only
xcopy "x64\Release\*" "<game folder>\" /s /y          # next to Recroom_Release.exe
# start Steam, log in
Install-PreserveLoader.bat                            # game closed
RecRoomPreserve.bat                                   # watch the logs, screenshot shows @scientist
# revert:
Restore-RefereeProtector.bat
```

The injector flow is byte-compatible with the pre-fix patch: with no proxy in the module
list, `GetPreserveProtector` does not exist and every code path takes the original branch.

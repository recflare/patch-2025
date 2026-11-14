# Rec Room 2025 Patch

**An archival patch for Rec Room (build 20250718.01, July 2025)** — repoints the
long-dead official backend at a mirror, bypasses the client's integrity checks, and
attaches without any game files being modified. Playable end-to-end against the
`recflare.net` mirror with Steam running.

> Credits: fork of [Carpetsoft/Rec-Room-2025-Patch](https://github.com/Carpetsoft/Rec-Room-2025-Patch).

![Rec Room title screen — logged in as @scientist](docs/images/title-screen.png)

**Status: verified working.** Boots to the title screen against the mirror,
in-session voice and networking, logged in as `@scientist`. See
[PRESERVATION.md](PRESERVATION.md) for the exact configuration and what is and isn't
guaranteed.

## What this is

Rec Room's service was shut down. The client build this targets still runs, but its
API hosts (`ns.rec.net` and friends) are gone, and its own anti-tamper layer refuses
to execute usefully against any other backend. This project is the preservation
layer:

- `2025Patch.dll` repoints the dead backend hosts at whatever you configure
  (`ApiHost` / `PhotonHost`), suppresses the obfuscated integrity checks
  ("Referee" checks), forces image-signature verification to pass, and blocks
  dead telemetry CDNs. No backend is compiled in; hosts default to empty (off).
- `PreserveLoader.dll` (shipped as `Referee.dll` by the installer) attaches the
  patch from inside the process — through the game's own static import — with **no
  remote threads**. It also loads every DLL found in `preserve\modules\`, which is
  the extension point for additional mods, in filename order.
- `Injector.exe` is the classic remote-thread injector, kept as the default for
  people who don't want to reshuffle files.
- `Metadump/` is an analysis tool for recovering the decrypted il2cpp metadata and
  method maps from the live process (dumps are never committed).
- `CLAUDE.md` is the long-form internal working document: every finding, every
  dead end, every address in this build, in one place.

## Quick start

Requirements: **Steam running and logged in**, the July 2025 build of Rec Room,
Windows x64, and a backend to point the client at (or accept that an empty
`2025patch.ini` boots past nothing).

```powershell
# 1. Build (Release|x64 only — Debug does not link)
.\build.ps1

# 2. Copy everything from x64\Release\ into the game folder (next to
#    Recroom_Release.exe). Steam up. Your choice of flow:

# Flow A (recommended) — preservation loader, no injector
Install-PreserveLoader.bat   # one-time, game closed: protector -> preserve\, loader -> Referee.dll
RecRoomPreserve.bat          # then play; every launch is like this

# Flow B — classic injector
RecRoomScreen.bat            # desktop; VR via RecRoomVR.bat
```

Full walkthrough, revert steps, and troubleshooting: **[PRESERVATION.md](PRESERVATION.md)**.

## What's in the repo

| path | what it is |
| --- | --- |
| `2025Patch/` | the patch DLL — DNS, SendRequest, CheatMgr, DUID, image-sig, voice hooks |
| `RefereeProxy/` | `PreserveLoader.dll` — import-binding proxy loader + module folder |
| `Injector/` | `Injector.exe` — classic remote-thread injector (alternative flow) |
| `Metadump/` | analysis tool: dumps the decrypted il2cpp image + method RVA map |
| `*.bat` | installer / launchers / one-click revert (`Install-`, `RecRoom`, `Restore-`) |
| `docs/images/` | verification screenshots |
| `PRESERVATION.md` | verified state, quick start, the module folder, honest Defender status |
| `CLAUDE.md` | internals: how the patch works, every finding, every rule of the build |
| `.github/workflows/build.yml` | CI: builds Release|x64 and cuts the drop-in zip on `v*` tags |

## Rules (learned the hard way)

- **Steam must be running.** The client dies silently at boot without it.
- Game files are never modified — the protector stays byte-identical at
  `preserve\Referee.dll`; the installer only relocates it.
- No game data, dumps, rebuilt metadata, or `client.zip` in this repo. All runtime
  artifacts land under `recroom\` which is gitignored.
- Releases ship the drop-in set only: two DLLs are the patch + loader; no game bytes.

## Limitations / honesty

- This exists to keep a dead game playable, not to bypass anything in a live
  service.
- The hooks are pinned to build 20250718.01; any rebuilt client breaks them.
- All testing is done with Defender **off**. The loader flow creates no remote
  threads (a strictly cleaner shape), but nothing here is guaranteed against a
  Defender-on machine. If Defender erases the files, exclude the game folder.

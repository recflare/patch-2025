# Rec Room 2025 Patch

**Credits:** Fork of https://github.com/Carpetsoft/Rec-Room-2025-Patch

This fork adds more support for photon settings and moves overrides into configuration
so it does not have to be rebuilt to change the name or photon server.

Compatible with build 20250718.01 (19 July 2025), Steam manifest 1151455856673601091.

- Disabled image signing, no need for sigs
- Added logging to investigate a Photon issue (probably remove later)
- Added custom .DLL injector

## Launching

The release zip unpacks into the Rec Room folder (the one with `Recroom_Release.exe`) as a flat
set: `2025Patch.dll`, `Injector.exe`, `2025patch.ini`, `RecRoomScreen.bat`, `RecRoomVR.bat`.

Run `RecRoomScreen.bat` (desktop) or `RecRoomVR.bat` (VR). Each starts `Injector.exe` first -- it
waits for `Recroom_Release.exe` and for `GameAssembly.dll` + `Referee.dll` to load before attaching
-- then launches the game with `+forcemode:screen` / `+forcemode:vr`. Both resolve every path
relative to themselves, so they only work from the game folder, and both refuse to do anything if
`Injector.exe` or `Recroom_Release.exe` is not sitting next to them.

Injecting by hand works the same way: start `Injector.exe`, then the game. Each injector patches
one client: it skips instances that already carry the patch, so re-running a launcher against a
running game is a no-op -- but launching a **second client** gets patched normally rather than
refused. Run a launcher again for each extra client you want.

Only the first client writes `2025patch.log`; the others log to `2025patch.<pid>.log` beside it, so
two sessions never shred each other's log.

There is no console window by default (Unity throttles the game whenever it loses focus, which
costs real room-load time) -- check `2025patch.log` in the game folder to confirm the patch
attached, or set `EnableConsole=true` in `2025patch.ini`.

## Configuration

`2025patch.ini` in this repo is a ready-to-use example carrying the built-in defaults, and it
ships in the release zip. Drop it next to `Recroom_Release.exe` (the same folder the patch writes
`2025patch.log` to) and edit as needed -- it is read once at injection.

If the file is absent the patch writes exactly this content itself on first run, so the copy in
the release is only there to make the knobs discoverable without launching first. An existing
file is never overwritten; delete a key (or the whole file) to fall back to the default.

No backend is compiled into the DLL: both hosts ship empty, so the server this patch talks to is
whatever you put in `ApiHost` / `PhotonHost`. With neither set, no request is redirected — the
Referee bypass and the rest of the patch still apply, the client just keeps talking to its own
(dead) hosts.

| key | default | effect |
| --- | --- | --- |
| `ApiHost` | *(empty)* | host that replaces `ns.rec.net` in the game's API request URIs; **empty = leave the URIs alone** |
| `PhotonHost` | *(empty)* | DNS target for `*.photonengine` / `exitgames` / `photonindustries`; **empty = leave Photon alone entirely** |
| `PhotonPort` | `0` | requires `PhotonHost`; port of the initial Photon name-server connect, `0` = Photon's protocol default. Master/game servers keep the ports the server hands out |
| `EnableConsole` | `false` | debug console window; costs load time (focus theft -> Unity throttling) |
| `BlockDeadHosts` | `true` | fail third-party telemetry/analytics lookups instantly |
| `SuppressDuidMismatch` | `true` | force the device-id mismatch check to false, fixing the launch / Create Account hang on a machine with a corrupt stored device id |
| `EnableTracing` | `false` | verbose diagnostic hooks; chatty, only for investigating |

Whatever ends up in effect is logged as `[Config] ...` in `2025patch.log`.

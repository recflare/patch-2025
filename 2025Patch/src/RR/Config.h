#pragma once
#include "../Utils/globals.h"

// =================================================================================================
// Runtime configuration -- 2025patch.ini, read from the game exe's folder (same place PatchLog
// writes 2025patch.log).
//
// The mirror hosts and the Photon backend selection used to be literals/constexpr in Patches.h, so
// pointing the patch at a different backend -- or A/B-ing against Photon Cloud -- meant a rebuild.
// They are read here instead, once, at DLL attach. NO BACKEND IS COMPILED IN: both host values
// start empty, so whichever server this patch talks to comes from the ini and nowhere else.
//
// INI + GetPrivateProfileString is deliberate: it is a Win32 API, so there is no parser to write and
// no dependency to add, and the file stays hand-editable next to the exe. If the file is missing it
// is created with the defaults below, so the knobs are discoverable without reading the source.
//
// Everything is best-effort -- a missing file, an unreadable one, or a malformed value all fall back
// to the compiled-in default rather than a nonsense port. For the two hosts that default is "unset",
// i.e. leave that piece of the client alone. Whatever is finally in effect is logged, so
// 2025patch.log always says which backend the run actually used.
// =================================================================================================
namespace RR::Config {

	constexpr const char* kFileName = "2025patch.ini";
	constexpr const char* kSection  = "config";

	// --- Defaults. Both hosts default to EMPTY: there is no compiled-in backend, so a build with no
	// ini (or with these keys blank) patches nothing network-wise and the client talks to whatever
	// its own URLs point at. Pointing it at a mirror is entirely a matter of filling these in. ---

	// Replaces ns.rec.net in BestHTTP request URIs.
	//
	// EMPTY IS THE OFF SWITCH: blank and SendRequest_H leaves every URI exactly as the game built it.
	// The host being REPLACED (ns.rec.net) is the dead official one and stays a literal in Patches.h;
	// only the replacement is configurable.
	char ApiHost[128] = "";

	// getaddrinfo target for *.photonengine / exitgames / photonindustries.
	//
	// EMPTY IS THE OFF SWITCH for everything Photon: no DNS redirect, no AppSettings write, and the
	// ConnectUsingSettings hook is not even installed -- the client talks to whatever Photon server
	// its backend hands it, untouched. There is nothing else to configure, because this client takes
	// its Photon app ids from an endpoint on the server rather than from AppSettings (see the PHOTON
	// BACKEND SELECTION block in Patches.h), so pointing it at a different Photon deployment is
	// purely a matter of swapping the server -- which is exactly what this one value does.
	char PhotonHost[128] = "";

	// Requires PhotonHost. 0 = leave Photon's protocol default (5058 UDP, 27000 alternative UDP).
	// This is the port of the INITIAL NAME-SERVER connect only -- it is written into the client at
	// GetNameServerAddress, since a port never passes through getaddrinfo. The master hands out its
	// own master/game-server addresses afterwards, ports included, so those come from the server.
	int PhotonPort = 0;

	// Diagnostic console (AllocConsole). Off by default and worth leaving off: the console window
	// steals foreground focus, Unity throttles hard while unfocused, and that measurably wrecked
	// room-load times -- see CreateConsole in main.cpp for the numbers. Everything it would print is
	// already in 2025patch.log, so this is only for live poking.
	bool EnableConsole = false;

	// THIRD-PARTY telemetry / analytics / crash-reporting hosts (RudderStack, Statsig, Backtrace,
	// Unity cloud). Most of these are still LIVE (verified 2026-08-17: statsigapi.net, submit.backtrace.io
	// and both cloud.unity3d.com hosts all resolve), so leaving this on is as much about not shipping
	// an archival session's telemetry and crash dumps to unrelated companies as it is about latency.
	// true = fail those lookups instantly at the getaddrinfo hook. Substrings are IsDeadHost in Patches.h.
	//
	// Backtrace is the one entry that is load-bearing rather than hygiene: its crash-upload backlog
	// starves the serial request queue until Photon disconnects (the ~50s silence then "Unable to
	// send message!"). Turning this off brings that back.
	//
	// ⚠️ Only ever list hosts that are genuinely external and unwanted. Blocking a *.recflare.net host
	// does NOT help: the client retries transport failures with backoff on the single serial
	// System.Net.Http queue, so a blocked host costs the same as an unreachable one. Only a real,
	// fast response stops the retries -- stub it server-side and drop it from the list instead.
	bool BlockDeadHosts = true;

	// DEVICE-ID (DUID) MISMATCH SUPPRESSION -- the fix for "the game will not launch" / Create Account
	// hanging on a machine whose STORED device id no longer matches the one derived at runtime.
	// true (default) = CheatManager.CheckForDUIDMismatch is forced to false, so the client never takes
	// the deviceId migration path it cannot get back out of. A no-op on healthy machines, which
	// already answer false. Set it false ONLY to observe a real mismatch -- that reproduces the hang.
	// See the DEVICE-ID (DUID) block in Patches.h; this is a workaround, the real fix is server-side.
	bool SuppressDuidMismatch = true;

	// VOICE (Tachyon) RSA PUBLIC KEY -- .NET <RSAKeyValue> XML, on ONE line.
	//
	// EMPTY IS THE OFF SWITCH: leave it blank and the client keeps Rec Room's baked-in key, exactly
	// as before. Set it to your own key pair's PUBLIC half and the voice server can decrypt the
	// handshake: RSA/PKCS#1 v1.5 unwrap CKA -> 32-byte AES key and CIA -> 16-byte IV, then
	// AES-256-CBC/PKCS7 decrypt AT to {"accountId","environment","accessToken"} and authenticate it.
	// (The client calls Encrypt(data, fOAEP: false), so the server must use PKCS#1 v1.5, not OAEP.)
	//
	// Any key size works -- the client only encrypts, so a 2048-bit key just makes CKA/CIA/VB 256
	// bytes instead of 128. Safe to paste RSA.ToXmlString(false) output verbatim.
	//
	// Self-test: VB is a fixed 16-byte constant encrypted with this same key, so after a swap it must
	// decrypt to 19 32 C5 68 80 28 9E 22 CA A8 71 09 14 88 5C 1B. If it does, the client is using
	// your key and AT is trustworthy; if it does not, the swap did not take.
	char VoiceKeyXml[2048] = "";

	// Diagnostic tracing: the request-pump probes ([Pump]/[Send]), Photon operation/status/event
	// tracers, HttpClient path logger and BestHTTP response logger. Off by default -- they are chatty
	// and every question they were built to answer is now recorded in CLAUDE.md and in the comment
	// blocks beside each hook. Kept rather than deleted because they are the only way this project has
	// ever managed to see inside the serial request queue or catch a server-pushed Photon event.
	bool EnableTracing = false;

	// Path to the ini, next to the game exe (Recroom_Release.exe) -- NOT the DLL and not the CWD.
	// GetPrivateProfileString requires a full path; given a bare name it searches the Windows
	// directory instead, which would silently ignore the user's file.
	static void ConfigPath(char* out, size_t cch) {
		GetModuleFileNameA(nullptr, out, (DWORD)cch);
		char* slash = strrchr(out, '\\');
		if (slash) strcpy_s(slash + 1, cch - (slash + 1 - out), kFileName);
	}

	static void Trim(char* s) {
		char* start = s;
		while (*start == ' ' || *start == '\t' || *start == '"') ++start;
		char* end = start + strlen(start);
		while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '"')) --end;
		*end = '\0';
		if (start != s) memmove(s, start, strlen(start) + 1);
	}

	// Host values additionally lose a scheme and any path: they are used as bare host names
	// (getaddrinfo takes a node name, not a URL), so "https://host/" has to become "host" rather than
	// fail at connect time. Returns false if nothing usable is left.
	static bool SanitizeHost(char* s) {
		Trim(s);
		char* start = s;
		if (_strnicmp(start, "https://", 8) == 0) start += 8;
		else if (_strnicmp(start, "http://", 7) == 0) start += 7;
		if (start != s) memmove(s, start, strlen(start) + 1);
		if (char* slash = strchr(s, '/')) *slash = '\0';
		Trim(s);
		return *s != '\0';
	}

	// EMPTY IS MEANINGFUL for both hosts -- each is the master switch for its own rewrite -- so
	// "ApiHost=" / "PhotonHost=" must CLEAR the value rather than keep it, and `offMeans` says in the
	// log what that switches off. The three cases are distinct on purpose: an absent key keeps the
	// current value (GetPrivateProfileString hands back the default we pass in, which is empty unless
	// something already set it), a blank one is an explicit opt-out, and a value that is present and
	// non-blank but sanitizes to nothing ("https://") is malformed input, not an opt-out, so it keeps
	// the current value too.
	//
	// The length check is not cosmetic: buf is wider than the destinations, and strcpy_s ABORTS the
	// process on overflow rather than truncating, so an over-long value has to be rejected like any
	// other bad input.
	static void ReadHost(const char* path, const char* key, char* value, size_t cch, const char* offMeans) {
		char buf[256] = {};
		GetPrivateProfileStringA(kSection, key, value, buf, (DWORD)sizeof(buf), path);
		char raw[256] = {};
		strcpy_s(raw, buf);
		Trim(raw);
		if (!*raw) {
			PatchLog("[Config] %s is empty -- %s", key, offMeans);
			*value = '\0';
			return;
		}
		if (!SanitizeHost(buf)) {
			PatchLog("[Config] %s is invalid, keeping %s", key, *value ? value : "(unset)");
			return;
		}
		if (strlen(buf) >= cch) {
			PatchLog("[Config] %s is too long (%zu chars), keeping %s", key, strlen(buf), *value ? value : "(unset)");
			return;
		}
		strcpy_s(value, cch, buf);
	}

	// Raw multi-hundred-char value (the RSA XML). Deliberately NOT SanitizeHost'd -- it is not a host
	// and contains '/' and '+' from base64. Only trimmed, length-checked, and shape-checked.
	//
	// The shape check is load-bearing rather than cosmetic: a malformed value would not fail here, it
	// would fail inside the game's own FromXmlString as a MANAGED exception thrown from a method we
	// hooked, and this project has already been bitten once by a managed exception unwinding through
	// a spoofed return address (STATUS_INVALID_DISPOSITION, see SendRequest_H). Rejecting obvious
	// garbage up front keeps a typo in an ini file from being a crash.
	static void ReadVoiceKey(const char* path, char* value, size_t cch) {
		char* buf = new (std::nothrow) char[4096];
		if (!buf) return;
		buf[0] = 0;
		GetPrivateProfileStringA(kSection, "VoiceKeyXml", "", buf, 4096, path);
		Trim(buf);
		if (!*buf) { delete[] buf; return; }           // absent/blank == keep Rec Room's key
		if (strlen(buf) >= cch) {
			PatchLog("[Config] VoiceKeyXml is too long (%zu chars, max %zu), keeping Rec Room's key",
				strlen(buf), cch - 1);
			delete[] buf;
			return;
		}
		if (!strstr(buf, "<RSAKeyValue>") || !strstr(buf, "<Modulus>") || !strstr(buf, "<Exponent>")) {
			PatchLog("[Config] VoiceKeyXml is not <RSAKeyValue> XML, keeping Rec Room's key");
			delete[] buf;
			return;
		}
		// A PRIVATE key would work too, but shipping one to every client is a mistake worth catching.
		if (strstr(buf, "<D>") || strstr(buf, "<InverseQ>")) {
			PatchLog("[Config] VoiceKeyXml contains PRIVATE key material -- refusing; use the public half only");
			delete[] buf;
			return;
		}
		strcpy_s(value, cch, buf);
		delete[] buf;
	}

	static void ReadBool(const char* path, const char* key, bool& value) {
		char buf[64] = {};
		GetPrivateProfileStringA(kSection, key, value ? "true" : "false", buf, (DWORD)sizeof(buf), path);
		Trim(buf);
		if (!_stricmp(buf, "true") || !_stricmp(buf, "1") || !_stricmp(buf, "yes") || !_stricmp(buf, "on")) {
			value = true;
		} else if (!_stricmp(buf, "false") || !_stricmp(buf, "0") || !_stricmp(buf, "no") || !_stricmp(buf, "off")) {
			value = false;
		} else {
			PatchLog("[Config] %s='%s' is not a boolean, keeping default %s", key, buf, value ? "true" : "false");
		}
	}

	static void ReadPort(const char* path, const char* key, int& value) {
		// GetPrivateProfileInt returns the default for a missing key and 0 for a non-numeric one; 0 is
		// also the legitimate "leave it alone" value here, so a typo'd port lands on the safe option.
		int v = (int)GetPrivateProfileIntA(kSection, key, value, path);
		if (v < 0 || v > 65535) {
			PatchLog("[Config] %s=%d out of range (0-65535), keeping default %d", key, v, value);
			return;
		}
		value = v;
	}

	// Written only when the file does not exist -- never overwrites a user's edits.
	static void WriteDefaultFile(const char* path) {
		FILE* f = nullptr;
		fopen_s(&f, path, "w");
		if (!f) return;
		fprintf(f,
			"; Rec Room 2025 patch configuration. Applied at injection.\n"
			"; Delete a line (or the whole file) to fall back to the built-in defaults. No backend is\n"
			"; baked in: leave both hosts blank and the patch makes no network changes at all.\n"
			"\n"
			"[%s]\n"
			"\n"
			"; Backend hosts. Bare host names -- no scheme, no path.\n"
			"; ApiHost rewrites ns.rec.net in the game's API requests. Leave it EMPTY to leave every\n"
			"; request URI exactly as the game built it.\n"
			"ApiHost=%s\n"
			"\n"
			"; PhotonHost is the DNS target for Photon (*.photonengine / exitgames / photonindustries).\n"
			"; Leave it EMPTY to make no Photon changes at all -- the client then connects to whatever\n"
			"; Photon server its backend gives it. The app ids always come from the server.\n"
			"PhotonHost=%s\n"
			"\n"
			"; Requires PhotonHost. Port of the initial Photon NAME-SERVER connect; 0 = leave Photon's\n"
			"; protocol default. The master/game servers are handed out by the server with their own ports.\n"
			"PhotonPort=%d\n"
			"\n"
			"; Debug console window. Costs load time -- it steals focus and Unity throttles while\n"
			"; unfocused. Everything it prints is already in 2025patch.log.\n"
			"EnableConsole=%s\n"
			"\n"
			"; Fail third-party telemetry/analytics host lookups (RudderStack, Statsig, Backtrace,\n"
			"; Unity cloud) instantly. Most are still live, so this also keeps an archival session's\n"
			"; telemetry and crash dumps from reaching unrelated companies. Do not add recflare hosts.\n"
			"BlockDeadHosts=%s\n"
			"\n"
			"; Force the device-id (DUID) mismatch check to false. Fixes the launch / Create Account\n"
			"; hang on a machine with a corrupt stored device id; a no-op on healthy machines.\n"
			"SuppressDuidMismatch=%s\n"
			"\n"
			"; Verbose diagnostic tracing: request-pump probes, Photon operation/event tracers,\n"
			"; HttpClient paths, BestHTTP responses. Chatty; only needed when investigating.\n"
			"EnableTracing=%s\n",
			kSection, ApiHost, PhotonHost, PhotonPort,
			EnableConsole ? "true" : "false", BlockDeadHosts ? "true" : "false",
			SuppressDuidMismatch ? "true" : "false", EnableTracing ? "true" : "false");
		fclose(f);
	}

	void Load() {
		char path[MAX_PATH] = {};
		ConfigPath(path, MAX_PATH);

		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
			WriteDefaultFile(path);
			PatchLog("[Config] %s not found; wrote defaults to %s", kFileName, path);
		}

		// The section was [hosts] before the Photon knobs were added. Such a file parses fine and
		// applies NOTHING, which looks identical to "config ignored" in the log -- so call it out.
		char legacy[256] = {};
		GetPrivateProfileStringA("hosts", "ApiHost", "", legacy, (DWORD)sizeof(legacy), path);
		if (*legacy) PatchLog("[Config] ignoring legacy [hosts] section -- rename it to [%s]", kSection);

		ReadHost(path, "ApiHost", ApiHost, sizeof(ApiHost), "leaving request URIs untouched");
		ReadHost(path, "PhotonHost", PhotonHost, sizeof(PhotonHost), "leaving Photon untouched");
		ReadBool(path, "EnableConsole",  EnableConsole);
		ReadBool(path, "BlockDeadHosts", BlockDeadHosts);
		ReadBool(path, "SuppressDuidMismatch", SuppressDuidMismatch);
		ReadBool(path, "EnableTracing",  EnableTracing);
		ReadPort(path, "PhotonPort",     PhotonPort);
		ReadVoiceKey(path, VoiceKeyXml, sizeof(VoiceKeyXml));

		PatchLog("[Config] %s", path);
		PatchLog("[Config] ApiHost=%s PhotonHost=%s PhotonPort=%d EnableConsole=%s BlockDeadHosts=%s"
			" SuppressDuidMismatch=%s EnableTracing=%s",
			*ApiHost ? ApiHost : "(none -- URIs untouched)",
			*PhotonHost ? PhotonHost : "(none -- Photon untouched)", PhotonPort,
			EnableConsole ? "true" : "false", BlockDeadHosts ? "true" : "false",
			SuppressDuidMismatch ? "true" : "false", EnableTracing ? "true" : "false");
		PatchLog("[Config] VoiceKeyXml=%s",
			*VoiceKeyXml ? "(set -- voice handshake re-keyed)" : "(none -- Rec Room's key kept)");
	}
}

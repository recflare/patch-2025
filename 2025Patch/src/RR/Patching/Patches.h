#pragma once
#include "../Methods.h"
#include "../Config.h"
#include "../../Utils/scan.h"
#include "../../Utils/deps/spoofcall/RetSpoof.hpp"
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Routine per-request chatter goes through TraceLog, anomalies keep using PatchLog. SendRequest_H is
// load-bearing (it performs the host rewrite) but logged 4 lines for EVERY request, which buried the
// interesting lines in a normal session. Gating only the routine ones keeps the log readable without
// touching behaviour.
#define TraceLog(...) do { if (RR::Config::EnableTracing) PatchLog(__VA_ARGS__); } while (0)

void (*nop_O)();
void nop_H() {
	return;
}

void (*SendRequest_O)(void*);
void SendRequest_H(void* request) {
	static long s_n = 0;
	long n = ++s_n;
	TraceLog("[SendRequest] #%ld enter request=%p", n, request);

	// Everything the hook does before forwarding is best-effort: it must NEVER take down the game.
	// An uncaught C++ exception here (bad_alloc, or std::string from a null pointer during the
	// injection/boot race) hits std::terminate -> the intermittent "Runtime Error! ...unusual way"
	// abort. So wrap the rewrite in try/catch and always fall through to the original request.
	try {
	void* uri = read<void*>(request, 16);

	if (uri) {

		using ToStringFn = Il2cppString * (*)(void*);
		auto fn = reinterpret_cast<ToStringFn>(GA + RR::Methods::System::Uri::ToString);

		Il2cppString* Uri = spoof_call(RetAddr, fn, uri);

		std::string find = "ns.rec.net";
		// The replacement nameserver (whatever serves the service map) comes from 2025patch.ini and
		// from nowhere else -- there is no compiled-in host, so an unset ApiHost means no rewrite at
		// all. The host being replaced is the dead official one, so it stays fixed.
		std::string replace = RR::Config::ApiHost;

		// ReadIl2CppString returns a new[]-allocated buffer or nullptr. `std::string s = nullptr` is
		// undefined behavior (this was the intermittent crash) -- guard it, and free the buffer (the
		// original code leaked it every request).
		char* raw = ReadIl2CppString(Uri);
		if (!raw) {
			PatchLog("[SendRequest] #%ld uri unreadable, passing through", n);
			SendRequest_O(request);
			TraceLog("[SendRequest] #%ld returned", n);
			return;
		}
		std::string url(raw);
		delete[] raw;

		TraceLog("[SendRequest] #%ld url=%s", n, url.c_str());
		// No ApiHost configured = no rewrite. Without this guard an empty value would splice the host
		// out of the URI entirely rather than leaving the request alone.
		if (!replace.empty() && url.find(find) != std::string::npos) {
			url.replace(url.find(find), find.length(), replace);
			TraceLog("[SendRequest] #%ld REWRITE -> %s", n, url.c_str());

			using Il2cppStringNewFn = Il2cppString * (*)(const char*);
			auto fn2 = reinterpret_cast<Il2cppStringNewFn>(GA + RR::Methods::Il2cpp::il2cpp_string_new);

			Il2cppString* NewStr = spoof_call(RetAddr, fn2, url.c_str());

			using ObjectGetClassFn = void * (*)(void*);
			auto fnyeah = reinterpret_cast<ObjectGetClassFn>(GA + RR::Methods::Il2cpp::il2cpp_object_get_class);

			void* UriClass = spoof_call(RetAddr, fnyeah, uri);

			using ObjectNewFn = void* (*)(void*);
			auto fnyeah2 = reinterpret_cast<ObjectNewFn>(GA + RR::Methods::Il2cpp::il2cpp_object_new);

			void* NewUri = spoof_call(RetAddr, fnyeah2, UriClass);

			if (NewUri && NewStr) {
				using UriCtor = void (*)(void*, Il2cppString*);
				auto fn = reinterpret_cast<UriCtor>(GA + RR::Methods::System::Uri::ctor);

				spoof_call(RetAddr, fn, NewUri, NewStr);

				set<void*>(request, 16, NewUri);

				uri = NewUri;
			}
		}

		// Log the (already-computed) url. The previous code did a SECOND spoof_call ToString here
		// purely to print the final URI -- that redundant spoofed il2cpp call was the crash site: a
		// managed exception unwinding through the spoofed return address yields STATUS_INVALID_
		// DISPOSITION (0xC0000026) and kills the process deep in home-screen loading. We already have
		// `url` (post-rewrite), so just print it -- no extra il2cpp call, no extra crash surface.
		std::cout << "Uri: " << url << std::endl;
	}
	}
	catch (...) {
		// Rewrite failed somehow -- never abort the game; just forward the request unchanged.
		PatchLog("[SendRequest] #%ld exception in hook, passing through unchanged", n);
	}

	TraceLog("[SendRequest] #%ld -> original", n);
	SendRequest_O(request);
	TraceLog("[SendRequest] #%ld returned", n);
	return;
}

// ---------------------------------------------------------------------------------------------
// Response logger: correlate URL -> HTTP status -> actual body.
//
// A Utf8Json failure ("expected:'Number Token', actual:'{'") names the expected TYPE but never the
// endpoint, so attributing a payload to a URL by eyeballing the body is guesswork. This hooks
// HTTPRequest.CallCallback (fires when the response is in hand) and logs the request pointer -- the
// same pointer SendRequest_H already logged next to the URL -- plus status and a body preview. That
// makes the mapping exact instead of inferred.
//
// Deliberately does NOT call Uri.ToString here: a second spoofed il2cpp call on a path that may be
// unwinding is what previously corrupted the unwind and killed the process (0xC0000026). The request
// pointer is enough to join against the SendRequest log line.
// ---------------------------------------------------------------------------------------------
void (*CallCallback_O)(void*);
void CallCallback_H(void* request) {
	// Run the real callback FIRST. HTTPResponse.Data is a lazily-populated backing field: reading it
	// at hook entry (before the client's own callback touches Data/DataAsText) returned null for
	// 142 of 143 responses. After the callback has run it is materialised.
	CallCallback_O(request);

	__try {
		if (request) {
			void* resp = read<void*>(request, RR::Offsets::Request_Response);
			int status = resp ? read<int32_t>(resp, RR::Offsets::Response_StatusCode) : -1;
			int len = 0;
			char preview[513] = {};
			if (resp) {
				void* data = read<void*>(resp, RR::Offsets::Response_Data);
				len = read<int32_t>(resp, RR::Offsets::Response_DataLength);
				if (data && len > 0) {
					const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data) + RR::Offsets::Array_Data;
					int n = len < 512 ? len : 512;
					for (int i = 0; i < n; i++) {
						char c = static_cast<char>(bytes[i]);
						preview[i] = (c >= 32 && c < 127) ? c : '.';
					}
					preview[n] = 0;
				}
				else {
					// Fallback: the string form, populated when the client reads DataAsText.
					Il2cppString* txt = read<Il2cppString*>(resp, RR::Offsets::Response_DataAsText);
					if (txt) {
						char* s = ReadIl2CppString(txt);
						if (s) {
							int i = 0; for (; s[i] && i < 512; i++) preview[i] = s[i];
							preview[i] = 0; len = i;
							delete[] s;
						}
					}
				}
			}
			PatchLog("[HTTP<-] request=%p status=%d len=%d body=%s", request, status, len, preview);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------------------------
// HttpClient request logger (RecNet's System.Net.Http path, CBACIMLIBPF.IBJKIAMJCDN).
//
// Both overloads are STATIC, so there is no `this`: arg1=HttpMethod*, arg2=service enum,
// arg3=path string (r8), rest follow. Every parameter is <=8 bytes (Nullable<int32> and
// CancellationToken are both 8), so declaring them as pointer-sized slots forwards correctly under
// the Win64 ABI. We only read the path and pass everything straight through.
// ---------------------------------------------------------------------------------------------
typedef void* (*Req9_t)(void*, uint64_t, Il2cppString*, void*, uint64_t, uint64_t, void*, void*, uint64_t, void*);
typedef void* (*Req10_t)(void*, uint64_t, Il2cppString*, void*, void*, uint64_t, uint64_t, void*, void*, uint64_t, void*);
Req9_t  Req9_O = nullptr;
Req10_t Req10_O = nullptr;

static void LogHttpClientPath(const char* which, uint64_t svc, Il2cppString* path) {
	__try {
		if (path) {
			char* p = ReadIl2CppString(path);
			if (p) { PatchLog("[HttpClient/%s] svc=%llu path=%s", which, svc, p); delete[] p; }
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* Req9_H(void* m, uint64_t svc, Il2cppString* path, void* a4, uint64_t a5, uint64_t a6, void* a7, void* a8, uint64_t a9, void* mi) {
	LogHttpClientPath("9", svc, path);
	return Req9_O(m, svc, path, a4, a5, a6, a7, a8, a9, mi);
}
void* Req10_H(void* m, uint64_t svc, Il2cppString* path, void* a4, void* a5, uint64_t a6, uint64_t a7, void* a8, void* a9, uint64_t a10, void* mi) {
	LogHttpClientPath("10", svc, path);
	return Req10_O(m, svc, path, a4, a5, a6, a7, a8, a9, a10, mi);
}

// ---------------------------------------------------------------------------------------------
// Request-pump probes -- locate exactly where the room-save blob dies.
//
// See RR::Methods::HttpClient in Methods.h. The blob is logged by Req9/Req10 (it IS enqueued) and
// then never reaches SendRequest, and it is not escaping via another transport: all 177 [HTTP<-]
// responses in the 2026-08-17 run matched a SendRequest, zero unmatched. These two hooks split the
// remaining gap in half.
//
// Both targets are async, so they return a Task immediately and these hooks only observe ENTRY --
// enough to answer "did this request get that far", which is the whole question. Pass every argument
// through untouched; the stack slots are modelled as uint64_t because each occupies one full slot
// regardless of declared width (bool, Nullable<int> and CancellationToken are all <=8 bytes).
// ---------------------------------------------------------------------------------------------
void* (*Pump_O)(void*, uint64_t, void*);
void* Pump_H(void* queue, uint64_t ct, void* mi) {
	// The pump ticking at all is the signal; log the queue identity so a stalled queue is visible as
	// "enqueued to X, but X never pumped again".
	PatchLog("[Pump] KOMEOKGFOBP queue=%p", queue);
	return Pump_O(queue, ct, mi);
}

void* (*Send_O)(uint32_t, void*, uint64_t, Il2cppString*, void*, uint64_t, uint64_t, void*, void*, uint64_t, void*);
void* Send_H(uint32_t attempt, void* method, uint64_t svc, Il2cppString* path, void* a5, uint64_t a6,
             uint64_t a7, void* a8, void* a9, uint64_t a10, void* mi) {
	__try {
		if (path) {
			char* p = ReadIl2CppString(path);
			if (p) { PatchLog("[Send] attempt=%u svc=%llu path=%s", attempt, svc, p); delete[] p; }
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return Send_O(attempt, method, svc, path, a5, a6, a7, a8, a9, a10, mi);
}

// ---------------------------------------------------------------------------------------------
// Force Photon PAYLOAD encryption (disable datagram encryption).
//
// See RR::Methods::Photon in Methods.h for the reasoning. Short version: Luxon only implements
// payload encryption; with datagram encryption every post-handshake packet fails its ENet CRC /
// protocol validation and is dropped WITHOUT a log, which is why the server showed "Established
// encryption" then 43s of silence while the client retried every 5.03s and finally threw
// "Unable to send message!". Returning false keeps the client on the payload path Luxon speaks.
//
// Logs the ORIGINAL value once, so we confirm the client really was asking for datagram encryption
// (orig=1) instead of assuming it.
// ---------------------------------------------------------------------------------------------
bool (*IsTransportEncrypted_O)(void*, void*);
bool IsTransportEncrypted_H(void* self, void* mi) {
	static bool logged = false;
	if (!logged) {
		logged = true;
		bool orig = false;
		__try { orig = IsTransportEncrypted_O(self, mi); } __except (EXCEPTION_EXECUTE_HANDLER) {}
		PatchLog("[Crypto] EnetPeer.IsTransportEncrypted original=%d -> forcing FALSE (payload encryption)", orig ? 1 : 0);
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Quit tracer -- names whoever asks the client to exit.
//
// See RR::Methods::AppLifecycle. The exit is orderly, not a crash, so there IS a caller; we log a
// native stack walk at the Running => PreparingToExit transition and convert each frame to a
// GameAssembly RVA, which can be looked up in the Cpp2IL dump to name the method. Frames outside
// GameAssembly are printed absolute so Unity/system callers stay distinguishable.
// ---------------------------------------------------------------------------------------------
// Resolve every frame to module+offset, not just GameAssembly. The GameAssembly-only version left
// the interesting half of the exit stack as bare addresses; naming the module is what shows the quit
// arriving from UnityPlayer rather than from game code. GameAssembly offsets are RVAs -- feed them to
//   py whatis2025.py --dump C:\Games\RecRoom_Info\Code\2025-07-19_02-45-25 <rva>
// or, for better coverage (346k methods vs the dump's 241k), look them up in il2cpp-2025/methods.pkl.
static void LogStack(const char* tag) {
	void* frames[24] = {};
	USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
	PatchLog("[%s] stack, %u frames", tag, n);
	for (USHORT i = 0; i < n; ++i) {
		uintptr_t a = (uintptr_t)frames[i];
		HMODULE m = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       (LPCSTR)a, &m) && m) {
			char path[MAX_PATH] = {};
			GetModuleFileNameA(m, path, MAX_PATH);
			const char* base = strrchr(path, '\\');
			base = base ? base + 1 : path;
			PatchLog("[%s]   #%u %s+0x%llX", tag, i, base, (unsigned long long)(a - (uintptr_t)m));
		} else {
			PatchLog("[%s]   #%u %p", tag, i, frames[i]);
		}
	}
}

void (*SetExitState_O)(int32_t, void*);
void SetExitState_H(int32_t state, void* mi) {
	__try {
		PatchLog("[Exit] AppExitState := %d (0=Running 1=PreparingToExit 2=ReadyForExit)", state);
		LogStack("Exit");
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	SetExitState_O(state, mi);
}

// UnityEngine.Application.Quit() / Quit(int).
//
// The exit stack proved the request enters managed code at Application.Internal_ApplicationWantsToQuit
// -- i.e. Unity's native layer already decided to quit before any Rec Room code ran. Two things reach
// that point: a managed Application.Quit() call, or a platform close request (WM_CLOSE / Alt+F4 /
// task kill). These hooks tell them apart: if [Quit] never fires yet the app still exits, nothing in
// managed code asked for it and the close came from the OS.
//
// One uniform 2-arg signature covers both overloads. In the x64 ABI a callee simply ignores register
// arguments it does not declare, so forwarding (a1, a2) is safe whether the real shape is
// (MethodInfo*) or (int exitCode, MethodInfo*).
void (*AppQuit_O)(uintptr_t, uintptr_t);
void AppQuit_H(uintptr_t a1, uintptr_t a2) {
	__try {
		PatchLog("[Quit] UnityEngine.Application.Quit called (a1=0x%llX)", (unsigned long long)a1);
		LogStack("Quit");
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	AppQuit_O(a1, a2);
}
void (*AppQuit2_O)(uintptr_t, uintptr_t);
void AppQuit2_H(uintptr_t a1, uintptr_t a2) {
	__try {
		PatchLog("[Quit] UnityEngine.Application.Quit (overload 2) called (a1=0x%llX)", (unsigned long long)a1);
		LogStack("Quit");
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	AppQuit2_O(a1, a2);
}

// ---------------------------------------------------------------------------------------------
// Anti-cheat module-scan suppressor -- see RR::Methods::AntiCheat for how this was found.
//
// CheatManager's scan spots our injected 2025Patch.dll among the loaded modules and calls this
// closure, whose ENTIRE body is SessionManager.FatalApplicationQuit(533223478, filenames). So this
// is a REPLACE-ONLY hook: we must never call the original, or the client quits anyway.
//
// We still log what it found, both to confirm the suppression works and because the filenames string
// is the only place the client tells us what it objects to -- if it ever lists something other than
// our DLL, that is worth seeing rather than silently swallowing.
//
// Note this suppresses the REACTION, not the detection: the scan still runs and still finds us.
// Hiding the module from the PEB loader list would defeat it at the source (recnet-patcher has a
// module_hide.c doing exactly that) and is the better fix if the scan ever grows a second consumer.
// ---------------------------------------------------------------------------------------------
void CheatQuit_H(void* self, void* mi) {
	__try {
		Il2cppString* fn = self ? *(Il2cppString**)((uint8_t*)self + 0x10) : nullptr;  // .filenames
		char* s = fn ? ReadIl2CppString(fn) : nullptr;
		PatchLog("[CheatMgr] module scan flagged \"%s\" -> FatalApplicationQuit SUPPRESSED",
			s ? s : "<null>");
		if (s) delete[] s;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	// Deliberately NOT calling the original.
}

// ---------------------------------------------------------------------------------------------
// DEVICE-ID (DUID) MISMATCH SUPPRESSOR -- replayed from recnet-patcher's DUIDMismatchPatch.cs
// (commit 6a62f0c, "Suppress DUID mismatch check resulting in create account hang").
//
// SYMPTOM: on a machine whose STORED device id no longer matches the one derived at runtime, the
// client hangs -- it "will not launch", or Create Account never completes, with nothing obviously
// wrong in the log. CheatManager.CheckForDUIDMismatch returns true, which sends the client down a
// migration path that POSTs PlayerReporting/v1/deviceId and then waits for a response it will
// accept. The archival server answers 200 {"success":true} and the client waits anyway: it never
// calls the create_account OAuth and never persists the id (WriteDUIDs never runs). Machines whose
// stored id matches never take the path at all, which is why it looks machine-specific.
//
// FIX: force the check to false, so the migration path is never entered. Healthy machines already
// answer false, so on them this changes nothing.
//
// (!) THIS IS A WORKAROUND, NOT A REPAIR. It lets the client through; it does not fix the stored id.
// The real fix is server-side -- make the endpoint stop reporting a stale old id so old == new and
// there is no mismatch to migrate. Two findings from recnet-patcher constrain anyone reopening this:
//
//   1. CLEARING LOCAL STORAGE DOES NOT FIX IT. The stored id lives in PlayerPrefs `cm_did_ppk`
//      (registry `cm_did_ppk_h3478365449`, CodeStage-obscured, so never plaintext). Deleting that
//      value did NOT change the oldDeviceId in the POST, and on the failing run `cm_did_ppk` was
//      never even read. So it is A copy, not the one that seeds the migration.
//   2. WHERE THE OLD ID ACTUALLY COMES FROM IS STILL UNKNOWN. It survives deleting the whole
//      HKCU\Software\Against Gravity\Rec Room key and appears nowhere as plaintext under
//      AppData/LocalLow. Leading theory: the backend recorded it from an earlier POST and hands it
//      back. Until that is found, suppression here or a correction server-side are the only fixes.
//
// The one diagnostic line below is aimed at (2): it runs the REAL check once and logs the stored id
// that came back. If a value ever appears there it is the first hard evidence of where the old id
// lives. Calling the original is safe -- the check only compares; the POST that hangs is issued by
// the CALLER once the check returns true. It is still wrapped in __try, because this method's entry
// is prologue-stolen (see RR::Methods::AntiCheat::CheckForDUIDMismatch) and the trampoline is the
// one part of this hook that depends on MinHook reconstructing that correctly.
// ---------------------------------------------------------------------------------------------
bool (*DuidMismatch_O)(void*, Il2cppString**, void*);
bool DuidMismatch_H(void* self, Il2cppString** stored, void* mi) {
	bool orig = false;
	Il2cppString* got = nullptr;
	__try { orig = DuidMismatch_O(self, &got, mi); }
	__except (EXCEPTION_EXECUTE_HANDLER) { orig = false; got = nullptr; }

	// Log the first call, and separately the first genuine mismatch -- the mismatch is the event worth
	// seeing but can happen long after the first call. Bounded at two lines either way; this runs on
	// CheatManager's own schedule and must not become per-frame chatter.
	static bool loggedOnce = false, loggedMismatch = false;
	if (!loggedOnce || (orig && !loggedMismatch)) {
		loggedOnce = true;
		if (orig) loggedMismatch = true;
		__try {
			char* s = got ? ReadIl2CppString(got) : nullptr;
			PatchLog("[DUID] CheatManager.CheckForDUIDMismatch original=%d stored=\"%s\" -> %s",
				orig ? 1 : 0, s ? s : "<null>",
				RR::Config::SuppressDuidMismatch ? "forcing FALSE (suppressed)" : "passing through");
			if (s) delete[] s;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	if (!RR::Config::SuppressDuidMismatch) {
		if (stored) *stored = got;
		return orig;
	}
	if (stored) *stored = nullptr;
	return false;
}

// ---------------------------------------------------------------------------------------------
// Rec Room shutdown API tracers. See RR::Methods::AppLifecycle.
//
// FatalApplicationQuit(int, string) is the prize: it is handed the reason as a plain string, so this
// turns "why did it quit" into a single log line instead of another stack walk. All static il2cpp
// methods, so MethodInfo* is the LAST argument.
// ---------------------------------------------------------------------------------------------
void (*TryQuit0_O)(void*);
void TryQuit0_H(void* mi) {
	__try { PatchLog("[Shutdown] TryApplicationQuit()"); LogStack("Shutdown"); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	TryQuit0_O(mi);
}

void (*TryQuit1_O)(int32_t, void*);
void TryQuit1_H(int32_t code, void* mi) {
	__try { PatchLog("[Shutdown] TryApplicationQuit(code=%d)", code); LogStack("Shutdown"); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	TryQuit1_O(code, mi);
}

void (*FatalQuit_O)(int32_t, Il2cppString*, void*);
void FatalQuit_H(int32_t code, Il2cppString* msg, void* mi) {
	__try {
		char* m = msg ? ReadIl2CppString(msg) : nullptr;
		PatchLog("[Shutdown] FatalApplicationQuit(code=%d, msg=\"%s\")", code, m ? m : "<null>");
		if (m) delete[] m;
		LogStack("Shutdown");
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	FatalQuit_O(code, msg, mi);
}

void* (*Logout_O)(void*);
void* Logout_H(void* mi) {
	__try { PatchLog("[Shutdown] LogoutToBootScene()"); LogStack("Shutdown"); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return Logout_O(mi);
}

void* (*LogoutAsync_O)(void*);
void* LogoutAsync_H(void* mi) {
	__try { PatchLog("[Shutdown] LogoutToBootSceneAsync()"); LogStack("Shutdown"); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return LogoutAsync_O(mi);
}

// ---------------------------------------------------------------------------------------------
// Image content-signature bypass.  See RR::Methods::ImageSignature in Methods.h for the full
// reasoning. Short version: mirrored images still carry rec.net's original `content-signature`
// RSA header, the client verifies it against a baked-in public key, and no mirror can ever satisfy
// it -- so the check is unsatisfiable rather than merely failing, and is dead weight for archival.
//
// Why this blocks ROOM LOAD and not just artwork: image fetches and the room save-data blob share
// one request pump (CBACIMLIBPF.KOMEOKGFOBP, which drains a Queue). In the failing run every svc=7
// request issued BEFORE the first signature failure (17:54:51.2) reached BestHTTP, and the first
// one issued after it -- the room blob, 17:54:52.4 -- never did; it is absent from both our
// SendRequest log and mitmproxy. That is a stalled pump, which is why the loading bar sits at
// DOWNLOADING_DETAILS until the 30s timeout cancels FetchRoomLoadDetails > getRoomSaveData.
//
// Logs the ORIGINAL verdict once (same discipline as IsTransportEncrypted above): if that line ever
// reads original=1 then signatures were verifying fine and this hook is masking a different fault.
// ---------------------------------------------------------------------------------------------
bool (*VerifyImageSig_O)(void*, void*, void*);
bool VerifyImageSig_H(void* data, void* sig, void* mi) {
	static bool logged = false;
	if (!logged) {
		logged = true;
		bool orig = false;
		__try { orig = VerifyImageSig_O(data, sig, mi); } __except (EXCEPTION_EXECUTE_HANDLER) {}
		PatchLog("[ImgSig] HPJKKCCECLH.BLDFMLMAFLH original=%d -> forcing TRUE", orig ? 1 : 0);
	}
	return true;
}

// =============================================================================================
// PHOTON BACKEND SELECTION
//
// The whole Photon story is ONE knob: RR::Config::PhotonHost (2025patch.ini; no compiled-in
// default, so unset means off), plus PhotonPort for the initial connect port.
//
//   PhotonHost set   -> DNS for *.photonengine / exitgames / photonindustries is redirected to it,
//                       and PhotonPort, if non-zero, overrides the connect port on AppSettings.
//   PhotonHost empty -> NOTHING here runs. No DNS redirect, no AppSettings write, and the
//                       ConnectUsingSettings hook is not installed at all. The client reaches
//                       whatever Photon server its backend points it at.
//
// Swapping the server is genuinely all there is to do, because the app ids are NOT ours to set:
// this client takes its Realtime/Voice/Chat ids from an endpoint on the server (Luxon hands out its
// own "rf-..." ids). There used to be a UsePhotonCloud flag with CloudAppIdRealtime/Voice/Chat and
// CloudFixedRegion knobs that wrote AppSettings.AppId* just before connect; all of it was removed
// after those writes were seen to achieve nothing. To reach a different Photon deployment, point
// the backend at it and set PhotonHost.
//
// ⚠️ The stated REASON for removing them was wrong, and it stayed wrong here for a long time. It
// was recorded as "the client never reads AppSettings.AppId*". It does read them -- in
// ConnectUsingSettings, which this client never calls, so the hook doing the writing never ran at
// all. Nothing on AppSettings is consumed by this client, app ids included. The same mistake later
// hid the PhotonPort bug (see the DEAD SEAM block in Methods.h): a hook that logs nothing is
// evidence about the hook, not about the field.
//
// What that flag was for, kept because the answer still stands: against Luxon the client joins, gets
// ~4 SetProperties answered, then receives NO responses for ~50s and dies with "Unable to send
// message!". The same client was run against real Photon Cloud to tell a Luxon bug from a
// client/patch bug -- Cloud reproduced the failure identically, proving Photon/Luxon are NOT the
// cause.
//
// ⚠️ The cause was the BACKLOG OF BACKTRACE UPLOADS, which piles up until Photon disconnects.
// submit.backtrace.io is in IsDeadHost below and BlockDeadHosts (default true) keeps that backlog
// from forming.
//
// An earlier version of this block blamed "the room's missing save data" instead. That was WRONG,
// and it is recorded here so it is not re-derived: rooms with no save blob load fine. The blob was
// the loudest thing in the log, which is exactly what made it a convincing wrong answer -- the queue
// is serial, so whatever is starving it shows up as the NEXT request failing, not as itself.
// =============================================================================================

// ---------------------------------------------------------------------------------------------
// Apply PhotonPort to the name-server connect.
//
// The Photon host is redirected at the DNS layer, but a PORT never passes through getaddrinfo, so a
// non-default one has to be written into the client instead. GetNameServerAddress is where that
// decision is made: it formats "<NameServerHost>:<port>" and takes the port from
// LoadBalancingClient.NameServerPortInAppSettings when that field is non-zero, otherwise from the
// protocol default (5058 UDP, 27000 alternative UDP). Writing the field immediately before the
// original runs therefore lands on every path that reaches the name server. The master still hands
// out its own master/game-server ports afterwards; this only moves the initial connect.
//
// THIS REPLACED A HOOK ON ConnectUsingSettings, WHICH NEVER RAN. That method reads AppSettings.Port
// and would have been the tidier seam, but this client never calls it -- the port silently stayed
// at 5058 and the hook logged nothing at all. The full evidence is in the DEAD SEAM block in
// Methods.h; the short version is that no [Photon] Port line ever appeared in a session log while
// the getaddrinfo redirect on the same connect logged fine. If this hook ever goes quiet the same
// way, that is the first thing to check -- the line below is unconditional on first call for
// exactly that reason.
//
// Only installed when PhotonHost AND PhotonPort are both set (see Patch()), so reaching this hook
// already means the caller wants the port changed. A managed string is returned by the original and
// simply passed through; nothing here calls back into il2cpp.
// ---------------------------------------------------------------------------------------------
void* (*GetNameServerAddress_O)(void*, void*);

void* GetNameServerAddress_H(void* self, void* mi) {
	__try {
		if (self && RR::Config::PhotonPort != 0) {
			int32_t was = read<int32_t>(self, RR::Offsets::LoadBalancingClient::NameServerPortInAppSettings);
			if (was != (int32_t)RR::Config::PhotonPort) {
				set<int32_t>(self, RR::Offsets::LoadBalancingClient::NameServerPortInAppSettings,
					(int32_t)RR::Config::PhotonPort);
				static bool logged = false;
				if (!logged) {
					logged = true;
					// 0 = the field was unset, i.e. the client was about to use the protocol default.
					PatchLog("[Photon] NameServer port %d -> %d (PhotonPort, host %s via DNS redirect)",
						(int)was, RR::Config::PhotonPort, RR::Config::PhotonHost);
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return GetNameServerAddress_O(self, mi);
}

bool IsPhotonHost(const std::string& node) {
	return node.find("photonengine") != std::string::npos
		|| node.find("exitgames") != std::string::npos
		|| node.find("photonindustries") != std::string::npos;
}

// Telemetry/analytics/crash/feature-flag hosts that do not exist on the archival backend. They never
// resolve, but every attempt still goes to the OS resolver and the client retries on a timer, and the
// System.Net.Http ones (RudderStack/Statsig/Unity) tie up connection-pool slots the room-load path
// then waits behind -- Player.log fills with "Curl error 6: Could not resolve host" and a storm of
// ServicePointScheduler.WaitAsync stalls. Failing the lookup ourselves (WSAHOST_NOT_FOUND) returns the
// SAME "not found" the resolver eventually would, just instantly, so the client's existing
// handle-the-failure path runs without the wait. Substring match -- these cover every subdomain seen
// (recroom-dataplane.rudderstack.com, submit.backtrace.io, cdp/perf-events/config.uca.cloud.unity3d.com).
// Gated by RR::Config::BlockDeadHosts (default true). NOTE: the list DOES include some *.recflare.net
// subdomains -- see the warning inside about why blocking those is a trade-off, not a free win.
bool IsDeadHost(const std::string& node) {
	static const char* kDeadHosts[] = {
		"rudderstack.com",    // RecNet analytics data plane
		"statsigapi.net",     // Statsig feature flags
		"backtrace.io",       // Backtrace crash upload -- LOAD-BEARING, not just telemetry hygiene:
		                      // its upload backlog is what starves the queue until Photon
		                      // disconnects. See the PHOTON BACKEND SELECTION block above.
		"cloud.unity3d.com",  // Unity analytics / perf-events / remote config (uca)

		// ⚠️ THIRD-PARTY HOSTS ONLY. Never add a *.recflare.net host here.
		//
		// These sit on CBACIMLIBPF's SINGLE SERIAL request queue -- the same one the room-save blob
		// (cdn.recflare.net/room/...) waits in, at roughly position 100. Blocking a host on that queue
		// is NOT free: the intuition "NXDOMAIN fails in milliseconds, so blocking costs nothing" is
		// wrong, because the cost is the client's RETRY BACKOFF, not the lookup. Measured 2026-08-17 --
		// one blocked `data/event` POST retried at ~5.5s and ~13.7s and held the queue for 24.68s, so
		// the blob was sent at 36.6s against a 30s room-load timeout and missed by ~6s. Blocked and
		// NXDOMAIN are equally bad; only an INSTANT response stops the backoff.
		//
		// All four recflare subdomains that used to be listed here were retired on 2026-08-17 once the
		// mirror implemented them (datacollection -> 200; cards / moderation / platformnotifications ->
		// fast 404, which is fine -- a definitive HTTP response does not trigger the transport-failure
		// backoff. config.recflare.net is still NXDOMAIN but unused, and blocking it changed nothing).
		// To retire any future entry the order matters: stub it server-side FIRST, then remove it here.
	};
	for (const char* h : kDeadHosts) {
		if (node.find(h) != std::string::npos) return true;
	}
	return false;
}

int (WSAAPI* getaddrinfo_O)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
int WSAAPI getaddrinfo_H(PCSTR pNodeName, PCSTR pServiceName, const ADDRINFOA* pHints, PADDRINFOA* ppResult) {
	if (RR::Config::BlockDeadHosts && pNodeName && IsDeadHost(pNodeName)) {
		PatchLog("[DNS] %s -> BLOCKED (dead telemetry host)", pNodeName);
		if (ppResult) *ppResult = nullptr;
		WSASetLastError(WSAHOST_NOT_FOUND);
		return WSAHOST_NOT_FOUND;
	}
	// No PhotonHost configured = no Photon swap; the lookup falls through to the log-and-forward path.
	if (*RR::Config::PhotonHost && pNodeName && IsPhotonHost(pNodeName)) {
		std::cout << "Photon: " << pNodeName << " -> " << RR::Config::PhotonHost << std::endl;
		PatchLog("[Photon] getaddrinfo %s -> %s", pNodeName, RR::Config::PhotonHost);
		return getaddrinfo_O(RR::Config::PhotonHost, pServiceName, pHints, ppResult);
	}
	// Log every other lookup too. The room-save download does NOT go through BestHTTP (it uses the
	// System.Net.Http client, CBACIMLIBPF), so our SendRequest hook is blind to it -- but its DNS
	// still comes through here, which is how we find out what host it is actually contacting.
	if (pNodeName) PatchLog("[DNS] %s", pNodeName);
	return getaddrinfo_O(pNodeName, pServiceName, pHints, ppResult);
}

int (WSAAPI* GetAddrInfoW_O)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
int WSAAPI GetAddrInfoW_H(PCWSTR pNodeName, PCWSTR pServiceName, const ADDRINFOW* pHints, PADDRINFOW* ppResult) {
	if (pNodeName) {
		char node[NI_MAXHOST] = {};
		size_t converted = 0;
		wcstombs_s(&converted, node, pNodeName, _TRUNCATE);

		if (RR::Config::BlockDeadHosts && IsDeadHost(node)) {
			PatchLog("[DNS] %s (W) -> BLOCKED (dead telemetry host)", node);
			if (ppResult) *ppResult = nullptr;
			WSASetLastError(WSAHOST_NOT_FOUND);
			return WSAHOST_NOT_FOUND;
		}
		if (*RR::Config::PhotonHost && IsPhotonHost(node)) {
			std::cout << "Photon: " << node << " -> " << RR::Config::PhotonHost << std::endl;

			PatchLog("[Photon] GetAddrInfoW %s -> %s", node, RR::Config::PhotonHost);
			wchar_t wideHost[NI_MAXHOST] = {};
			mbstowcs_s(&converted, wideHost, RR::Config::PhotonHost, _TRUNCATE);
			return GetAddrInfoW_O(wideHost, pServiceName, pHints, ppResult);
		}
		PatchLog("[DNS] %s (W)", node);
	}
	return GetAddrInfoW_O(pNodeName, pServiceName, pHints, ppResult);
}

// ---------------------------------------------------------------------------------------------
// Photon protocol tracer (diagnostic). Luxon works with the 2023 client but the 2025 client reaches
// the game server (5056) and then times out with "Unable to connect to game session" -- and Luxon
// logs no error, meaning the 2025 client is waiting on an operation/response Luxon doesn't provide.
// These two call-through hooks (RVAs from the RuntimeDump, build verified via SendRequest=0x77E0950)
// print every operation the client SENDS and every connection STATUS it gets, so we can see exactly
// what the 2025 client does that the 2023 client didn't:
//   PhotonPeer.SendOperation(byte opCode, Dictionary, SendOptions)  GA+0x7534610
//   PeerBase.EnqueueStatusCallback(StatusCode)                      GA+0x752C540
// ---------------------------------------------------------------------------------------------

static const char* PhotonOpName(unsigned op) {
	switch (op) {
	case 219: return "ExchangeKeysForEncryption";
	case 220: return "GetRegions";
	case 221: return "WebRpc";
	case 222: return "GetGameList";
	case 224: return "FindFriends";
	case 225: return "JoinRandomGame";
	case 226: return "JoinGame(JoinRoom)";
	case 227: return "CreateGame";
	case 228: return "LeaveLobby";
	case 229: return "JoinLobby";
	case 230: return "Authenticate";
	case 231: return "AuthenticateOnce";
	case 248: return "ChangeGroups";
	case 250: return "Rpc";
	case 251: return "GetProperties";
	case 252: return "SetProperties";
	case 253: return "RaiseEvent";
	case 254: return "Leave";
	case 255: return "Join(legacy)";
	default:  return "?";
	}
}

static const char* PhotonStatusName(unsigned s) {
	switch (s) {
	case 1022: return "SecurityExceptionOnConnect";
	case 1023: return "ExceptionOnConnect";
	case 1024: return "Connect";
	case 1025: return "Disconnect";
	case 1026: return "Exception";
	case 1030: return "SendError";
	case 1039: return "ExceptionOnReceive";
	case 1040: return "TimeoutDisconnect";
	case 1041: return "DisconnectByServerTimeout";
	case 1042: return "DisconnectByServerUserLimit";
	case 1043: return "DisconnectByServerLogic";
	case 1044: return "DisconnectByServerReasonUnknown";
	case 1048: return "EncryptionEstablished";
	case 1049: return "EncryptionFailedToEstablish";
	case 1050: return "ServerAddressInvalid";
	case 1051: return "DnsExceptionOnConnect";
	default:   return "?";
	}
}

// PhotonPeer.SendOperation(this, byte opCode, Dictionary* params, SendOptions, MethodInfo*) -> bool.
// SendOptions is <=8 bytes (fits R9); forward all args verbatim.
typedef bool (*SendOp_t)(void* self, uint64_t opCode, void* params, uint64_t sendOptions, void* mi);
SendOp_t SendOp_O = nullptr;
bool SendOp_H(void* self, uint64_t opCode, void* params, uint64_t sendOptions, void* mi) {
	unsigned op = (unsigned)(opCode & 0xFF);
	// Log the RESULT, not just the attempt. SendOperation returns false when the peer refuses the
	// send (not connected, outgoing queue full, or payload over the max size) -- nothing reaches the
	// wire and no response can ever come back. Photon Cloud and the self-hosted server BOTH stop
	// answering these identically, which only makes sense if the ops are never actually sent; then
	// "Unable to send message!" means exactly what it says.
	bool ok = SendOp_O(self, opCode, params, sendOptions, mi);
	PatchLog("[Photon] --> SendOperation op=%u (%s) peer=%p sent=%d", op, PhotonOpName(op), self, ok ? 1 : 0);
	return ok;
}

// PeerBase.EnqueueStatusCallback(this, StatusCode statusValue, MethodInfo*) -> void.
typedef void (*EnqStatus_t)(void* self, uint32_t status, void* mi);
EnqStatus_t EnqStatus_O = nullptr;
void EnqStatus_H(void* self, uint32_t status, void* mi) {
	PatchLog("[Photon] <-- Status %u (%s) peer=%p", status, PhotonStatusName(status), self);
	EnqStatus_O(self, status, mi);
}

// OperationResponse layout (ExitGames.Client.Photon.OperationResponse):
//   OperationCode uint8_t @16, ReturnCode int16_t @18, DebugMessage Il2CppString* @24
// Log every response the SERVER sends, decoded, so we can see if Luxon returns an error code the
// client treats as fatal -- and WHICH wire protocol is active (Protocol16 vs Protocol18; the 2023
// vs 2025 client may negotiate different serialization, a prime suspect for "2023 works, 2025 not").
static void LogOpResponse(const char* proto, void* resp) {
	if (!resp) return;
	unsigned opc = *(uint8_t*)((uint8_t*)resp + 16);
	short  rc   = *(int16_t*)((uint8_t*)resp + 18);
	Il2cppString* dbg = *(Il2cppString**)((uint8_t*)resp + 24);
	char* msg = dbg ? ReadIl2CppString(dbg) : nullptr;
	PatchLog("[Photon] <== Resp[%s] op=%u (%s) returnCode=%d%s%s", proto, opc, PhotonOpName(opc), (int)rc,
		msg ? " msg=" : "", msg ? msg : "");
	if (msg) delete[] msg;
}

// Incoming EVENTS -- a separate path from operation responses, and previously a blind spot: the
// tracer logged outgoing ops, status callbacks and op responses, so a server-pushed event was
// invisible. Photon reserves code 251 = ErrorInfo, which is exactly how a server tells a client
// something is wrong, so "did Photon ask us to quit?" was unanswerable without this.
//
// EventData layout (ExitGames.Client.Photon.EventData): Code uint8_t @16.
static const char* PhotonEventName(unsigned code) {
	switch (code) {
		case 251: return "ErrorInfo";
		case 252: return "AzureNodeInfo";
		case 253: return "PropertiesChanged";
		case 254: return "Leave";
		case 255: return "Join";
		default:  return "game-event";
	}
}
static void LogEventData(const char* proto, void* ev) {
	if (!ev) return;
	unsigned code = *(uint8_t*)((uint8_t*)ev + 16);
	PatchLog("[Photon] <<= Event[%s] code=%u (%s)", proto, code, PhotonEventName(code));
}

typedef void* (*DeserEvent_t)(void* self, void* stream, int flags, void* mi);
DeserEvent_t Ev16_O = nullptr, Ev18_O = nullptr;
void* Ev16_H(void* self, void* stream, int flags, void* mi) {
	void* r = Ev16_O(self, stream, flags, mi);
	__try { LogEventData("P16", r); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	return r;
}
void* Ev18_H(void* self, void* stream, int flags, void* mi) {
	void* r = Ev18_O(self, stream, flags, mi);
	__try { LogEventData("P18", r); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	return r;
}

typedef void* (*DeserResp_t)(void* self, void* stream, int flags, void* mi);
DeserResp_t Deser16_O = nullptr, Deser18_O = nullptr;
void* Deser16_H(void* self, void* stream, int flags, void* mi) {
	void* r = Deser16_O(self, stream, flags, mi);
	__try { LogOpResponse("P16", r); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	return r;
}
void* Deser18_H(void* self, void* stream, int flags, void* mi) {
	void* r = Deser18_O(self, stream, flags, mi);
	__try { LogOpResponse("P18", r); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	return r;
}

namespace RR::Methods::Photon {
	uintptr_t SendOperation = 0x7534610;
	uintptr_t EnqueueStatusCallback = 0x752C540;
	uintptr_t Protocol16_DeserializeOperationResponse = 0x75388D0;
	uintptr_t Protocol18_DeserializeOperationResponse = 0x753E610;
	// Server-pushed events (RVAs from il2cpp-2025/methods.pkl).
	uintptr_t Protocol16_DeserializeEventData = 0x7537E30;
	uintptr_t Protocol18_DeserializeEventData = 0x753E2F0;
}

// =================================================================================================
// VOICE HANDSHAKE RE-KEY -- give the voice server a key it can actually decrypt with.
//
// TachyonClient's connect payload is {AI, AT, VB, CKA, CIA, CPK}. Only AI (the account id) is
// plaintext, and it is client-asserted, so a self-hosted voice server has nothing it can trust: AT
// (the Photon auth blob, which carries accountId/environment/accessToken) is AES-encrypted, and the
// AES key and IV travel as CKA/CIA RSA-encrypted to a public key baked into the client. Reading any
// of it needs Rec Room's private key.
//
// Swapping that public key for one of ours fixes it in a single field write. The ctor reads the XML
// from DMIBBCKIGCG's statics immediately before calling rsa.FromXmlString (see Methods.h), so we
// overwrite the static and let the game do the import itself -- deliberately, because the
// alternative (calling FromXmlString ourselves) would put a managed method that CAN THROW behind a
// spoofed return address, which is exactly the crash SendRequest_H documents.
//
// Ordering is the whole trick. The ctor runs DMIBBCKIGCG's .cctor a few instructions before it reads
// the static (cctor check at 0x82796C3), and that .cctor is what installs the stock key -- so a
// naive write before the original would simply be overwritten. Forcing the class init ourselves
// first means the game's own check finds it already initialised, skips it, and reads our value.
//
// Off unless VoiceKeyXml is set in 2025patch.ini, and every failure path leaves Rec Room's key in
// place, so the worst case is voice exactly as it behaved before.
// =================================================================================================
void (*TachyonCtor_O)(void*, void*);

static void ApplyVoiceKey() {
	__try {
		auto klass = *reinterpret_cast<uint8_t**>(GA + RR::Offsets::Tachyon::KeyHolderTypeInfo);
		// The slot holds an encoded metadata index until il2cpp resolves it; by the time a
		// TachyonClient is constructed it is a real pointer, but check rather than trust.
		if (reinterpret_cast<uintptr_t>(klass) < 0x10000) {
			PatchLog("[Voice] key holder not resolved yet (slot=%p) -- Rec Room's key kept", klass);
			return;
		}

		using ClassInitFn = void (*)(void*);
		auto init = reinterpret_cast<ClassInitFn>(GA + RR::Methods::Il2cpp::il2cpp_runtime_class_init);
		spoof_call(RetAddr, init, reinterpret_cast<void*>(klass));

		auto statics = read<uint8_t*>(klass, RR::Offsets::Tachyon::Class_StaticFields);
		if (!statics) {
			PatchLog("[Voice] key holder has no static block -- Rec Room's key kept");
			return;
		}

		using Il2cppStringNewFn = Il2cppString * (*)(const char*);
		auto mk = reinterpret_cast<Il2cppStringNewFn>(GA + RR::Methods::Il2cpp::il2cpp_string_new);
		Il2cppString* xml = spoof_call(RetAddr, mk, static_cast<const char*>(RR::Config::VoiceKeyXml));
		if (!xml) {
			PatchLog("[Voice] il2cpp_string_new failed -- Rec Room's key kept");
			return;
		}

		set<void*>(statics, RR::Offsets::Tachyon::Static_ServerKeyXml, xml);
		PatchLog("[Voice] handshake re-keyed from VoiceKeyXml (%zu chars) -- CKA/CIA/AT are now "
			"decryptable with your private key", strlen(RR::Config::VoiceKeyXml));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		PatchLog("[Voice] re-key FAILED (exception) -- Rec Room's key still in effect");
	}
}

void TachyonCtor_H(void* self, void* deps) {
	static bool s_logged = false;
	if (*RR::Config::VoiceKeyXml) {
		ApplyVoiceKey();
	} else if (!s_logged) {
		s_logged = true;
		PatchLog("[Voice] VoiceKeyXml not set -- handshake stays on Rec Room's key (server cannot read AT)");
	}
	TachyonCtor_O(self, deps);
}

namespace RR::Patches {
	void Resolve() {

		RetAddr = PatternScan<uint64_t*>("FF 23", GA, NtHeaders->OptionalHeader.SizeOfImage);
		auto assm = GetModuleHandle("GameAssembly.dll");

		RR::Methods::Il2cpp::il2cpp_string_new = reinterpret_cast<uintptr_t>(spoof_call(RetAddr, GetProcAddress, assm, "il2cpp_string_new")) - GA;
		RR::Methods::Il2cpp::il2cpp_object_get_class = reinterpret_cast<uintptr_t>(spoof_call(RetAddr, GetProcAddress, assm, "il2cpp_object_get_class")) - GA;
		RR::Methods::Il2cpp::il2cpp_object_new = reinterpret_cast<uintptr_t>(spoof_call(RetAddr, GetProcAddress, assm, "il2cpp_object_new")) - GA;
		RR::Methods::Il2cpp::il2cpp_runtime_class_init = reinterpret_cast<uintptr_t>(spoof_call(RetAddr, GetProcAddress, assm, "il2cpp_runtime_class_init")) - GA;

		// To the log, not just the console -- the console is off by default now (see CreateConsole in
		// main.cpp), and a zero here means an export failed to resolve, which is worth keeping.
		PatchLog("[Resolve] il2cpp_string_new=0x%llX il2cpp_object_get_class=0x%llX il2cpp_object_new=0x%llX il2cpp_runtime_class_init=0x%llX",
			(unsigned long long)RR::Methods::Il2cpp::il2cpp_string_new,
			(unsigned long long)RR::Methods::Il2cpp::il2cpp_object_get_class,
			(unsigned long long)RR::Methods::Il2cpp::il2cpp_object_new,
			(unsigned long long)RR::Methods::Il2cpp::il2cpp_runtime_class_init);
	}

	void Patch() {

		MH_Initialize();

		MH_CreateHook((void*)(Referee + RR::Methods::Referee::Check1), &nop_H, (LPVOID*)&nop_O);
		MH_EnableHook((void*)(Referee + RR::Methods::Referee::Check1));

		MH_CreateHook((void*)(Referee + RR::Methods::Referee::Check2), &nop_H, (LPVOID*)&nop_O);
		MH_EnableHook((void*)(Referee + RR::Methods::Referee::Check2));

		MH_CreateHook((void*)(Referee + RR::Methods::Referee::Check3), &nop_H, (LPVOID*)&nop_O);
		MH_EnableHook((void*)(Referee + RR::Methods::Referee::Check3));

		MH_CreateHook((void*)(Referee + RR::Methods::Referee::Check4), &nop_H, (LPVOID*)&nop_O);
		MH_EnableHook((void*)(Referee + RR::Methods::Referee::Check4));

		MH_CreateHook((void*)(GA + RR::Methods::LegacyTlsAuthentication::NotifyServerCertificate), &nop_H, (LPVOID*)&nop_O);
		MH_EnableHook((void*)(GA + RR::Methods::LegacyTlsAuthentication::NotifyServerCertificate));

		MH_CreateHook((void*)(GA + RR::Methods::HTTPRequest::SendRequest), &SendRequest_H, (LPVOID*)&SendRequest_O);
		MH_EnableHook((void*)(GA + RR::Methods::HTTPRequest::SendRequest));

		// Anti-cheat module-scan suppressor. THIS is the fix for the idle "auto logout + quit";
		// replace-only, since the original does nothing but fatally quit.
		MH_CreateHook((void*)(GA + RR::Methods::AntiCheat::ModuleScanDetected), &CheatQuit_H, (LPVOID*)&nop_O);
		MH_EnableHook((void*)(GA + RR::Methods::AntiCheat::ModuleScanDetected));

		// Device-id (DUID) mismatch suppressor -- THIS is the fix for the client hanging at launch /
		// Create Account on a machine with a corrupt stored device id. Hooked unconditionally so the one
		// [DUID] line is logged either way; SuppressDuidMismatch decides whether the result is forced.
		MH_CreateHook((void*)(GA + RR::Methods::AntiCheat::CheckForDUIDMismatch), &DuidMismatch_H, (LPVOID*)&DuidMismatch_O);
		MH_EnableHook((void*)(GA + RR::Methods::AntiCheat::CheckForDUIDMismatch));

		// Image content-signature bypass -- mirrored assets can never satisfy rec.net's RSA signature,
		// and the resulting throw stalls the shared request pump that the room save-data blob is
		// queued behind. Install BEFORE anything fetches an image.
		MH_CreateHook((void*)(GA + RR::Methods::ImageSignature::Verify), &VerifyImageSig_H, (LPVOID*)&VerifyImageSig_O);
		MH_EnableHook((void*)(GA + RR::Methods::ImageSignature::Verify));

		// Voice handshake re-key. Always hooked (it is also the only place that logs which key the
		// run used); the hook itself no-ops unless VoiceKeyXml is set. Must be in place before the
		// first TachyonClient is constructed, which is why it goes in at patch time rather than
		// lazily on the voice path.
		MH_CreateHook((void*)(GA + RR::Methods::Tachyon::Ctor), &TachyonCtor_H, (LPVOID*)&TachyonCtor_O);
		MH_EnableHook((void*)(GA + RR::Methods::Tachyon::Ctor));

		// photon doesn't go through HTTPRequest, it resolves ns.photonengine.io etc. and connects over raw sockets,
		// so redirect it at the winsock dns level instead
		LoadLibraryA("ws2_32.dll");

		void* getaddrinfoTarget = nullptr;
		MH_CreateHookApiEx(L"ws2_32", "getaddrinfo", &getaddrinfo_H, (LPVOID*)&getaddrinfo_O, &getaddrinfoTarget);
		MH_EnableHook(getaddrinfoTarget);

		void* GetAddrInfoWTarget = nullptr;
		MH_CreateHookApiEx(L"ws2_32", "GetAddrInfoW", &GetAddrInfoW_H, (LPVOID*)&GetAddrInfoW_O, &GetAddrInfoWTarget);
		MH_EnableHook(GetAddrInfoWTarget);

		// Photon connect port. Only needed when a custom Photon server AND a non-default port are
		// both configured -- with no PhotonHost the patch leaves Photon entirely alone, so the hook
		// is not installed rather than installed as a no-op.
		const bool photonConnectHook = *RR::Config::PhotonHost && RR::Config::PhotonPort != 0;
		if (photonConnectHook) {
			MH_CreateHook((void*)(GA + RR::Methods::Photon::GetNameServerAddress), &GetNameServerAddress_H, (LPVOID*)&GetNameServerAddress_O);
			MH_EnableHook((void*)(GA + RR::Methods::Photon::GetNameServerAddress));
		}

		// NOT hooked: EnetPeer.IsTransportEncrypted. Measured original=0, i.e. the client was ALREADY
		// using payload encryption, so datagram encryption was never the cause of Luxon's silence.
		// Kept only as a recorded dead end so it is not re-tried.

		// ---- Diagnostic tracing (EnableTracing in 2025patch.ini, default false) --------------
		// None of these change behaviour; they only log. Kept rather than deleted because they
		// are the only way to see inside the serial request queue or catch a server-pushed
		// Photon event -- see the comment blocks beside each hook for what each one answered.
		if (RR::Config::EnableTracing) {
			// Response logger -- joins to the SendRequest line by request pointer.
			MH_CreateHook((void*)(GA + RR::Methods::HTTPRequest::CallCallback), &CallCallback_H, (LPVOID*)&CallCallback_O);
			MH_EnableHook((void*)(GA + RR::Methods::HTTPRequest::CallCallback));

			// HttpClient path logger -- this is where the room save/asset downloads actually go.
			MH_CreateHook((void*)(GA + RR::Methods::HttpClient::Request9), &Req9_H, (LPVOID*)&Req9_O);
			MH_EnableHook((void*)(GA + RR::Methods::HttpClient::Request9));

			MH_CreateHook((void*)(GA + RR::Methods::HttpClient::Request10), &Req10_H, (LPVOID*)&Req10_O);
			MH_EnableHook((void*)(GA + RR::Methods::HttpClient::Request10));

			// Quit tracer -- diagnostic only.
			MH_CreateHook((void*)(GA + RR::Methods::AppLifecycle::SetExitState), &SetExitState_H, (LPVOID*)&SetExitState_O);
			MH_EnableHook((void*)(GA + RR::Methods::AppLifecycle::SetExitState));

			MH_CreateHook((void*)(GA + RR::Methods::AppLifecycle::ApplicationQuit), &AppQuit_H, (LPVOID*)&AppQuit_O);
			MH_EnableHook((void*)(GA + RR::Methods::AppLifecycle::ApplicationQuit));

			MH_CreateHook((void*)(GA + RR::Methods::AppLifecycle::ApplicationQuit2), &AppQuit2_H, (LPVOID*)&AppQuit2_O);
			MH_EnableHook((void*)(GA + RR::Methods::AppLifecycle::ApplicationQuit2));

			// Rec Room shutdown API -- FatalApplicationQuit carries the reason string.
			MH_CreateHook((void*)(GA + RR::Methods::AppLifecycle::TryApplicationQuit0), &TryQuit0_H, (LPVOID*)&TryQuit0_O);
			MH_EnableHook((void*)(GA + RR::Methods::AppLifecycle::TryApplicationQuit0));

			MH_CreateHook((void*)(GA + RR::Methods::AppLifecycle::TryApplicationQuit1), &TryQuit1_H, (LPVOID*)&TryQuit1_O);
			MH_EnableHook((void*)(GA + RR::Methods::AppLifecycle::TryApplicationQuit1));

			MH_CreateHook((void*)(GA + RR::Methods::AppLifecycle::FatalApplicationQuit), &FatalQuit_H, (LPVOID*)&FatalQuit_O);
			MH_EnableHook((void*)(GA + RR::Methods::AppLifecycle::FatalApplicationQuit));

			MH_CreateHook((void*)(GA + RR::Methods::AppLifecycle::LogoutToBootScene), &Logout_H, (LPVOID*)&Logout_O);
			MH_EnableHook((void*)(GA + RR::Methods::AppLifecycle::LogoutToBootScene));

			MH_CreateHook((void*)(GA + RR::Methods::AppLifecycle::LogoutToBootSceneAsync), &LogoutAsync_H, (LPVOID*)&LogoutAsync_O);
			MH_EnableHook((void*)(GA + RR::Methods::AppLifecycle::LogoutToBootSceneAsync));

			// Pump probes -- diagnostic only, safe to remove once the blob stall is understood.
			MH_CreateHook((void*)(GA + RR::Methods::HttpClient::Pump), &Pump_H, (LPVOID*)&Pump_O);
			MH_EnableHook((void*)(GA + RR::Methods::HttpClient::Pump));

			MH_CreateHook((void*)(GA + RR::Methods::HttpClient::Send), &Send_H, (LPVOID*)&Send_O);
			MH_EnableHook((void*)(GA + RR::Methods::HttpClient::Send));

			// Photon protocol tracer -- logs every operation sent + every connection status, to diagnose
			// the game-session join hang against Luxon.
			MH_CreateHook((void*)(GA + RR::Methods::Photon::SendOperation), &SendOp_H, (LPVOID*)&SendOp_O);
			MH_EnableHook((void*)(GA + RR::Methods::Photon::SendOperation));

			MH_CreateHook((void*)(GA + RR::Methods::Photon::EnqueueStatusCallback), &EnqStatus_H, (LPVOID*)&EnqStatus_O);
			MH_EnableHook((void*)(GA + RR::Methods::Photon::EnqueueStatusCallback));

			// Incoming operation-response tracer (both protocol impls; whichever fires reveals the active
			// serialization AND the server's return codes).
			MH_CreateHook((void*)(GA + RR::Methods::Photon::Protocol16_DeserializeOperationResponse), &Deser16_H, (LPVOID*)&Deser16_O);
			MH_EnableHook((void*)(GA + RR::Methods::Photon::Protocol16_DeserializeOperationResponse));

			MH_CreateHook((void*)(GA + RR::Methods::Photon::Protocol18_DeserializeOperationResponse), &Deser18_H, (LPVOID*)&Deser18_O);
			MH_EnableHook((void*)(GA + RR::Methods::Photon::Protocol18_DeserializeOperationResponse));

			// Server-pushed events -- covers ErrorInfo (251), the one way Photon could ask us to quit.
			MH_CreateHook((void*)(GA + RR::Methods::Photon::Protocol16_DeserializeEventData), &Ev16_H, (LPVOID*)&Ev16_O);
			MH_EnableHook((void*)(GA + RR::Methods::Photon::Protocol16_DeserializeEventData));

			MH_CreateHook((void*)(GA + RR::Methods::Photon::Protocol18_DeserializeEventData), &Ev18_H, (LPVOID*)&Ev18_O);
			MH_EnableHook((void*)(GA + RR::Methods::Photon::Protocol18_DeserializeEventData));

		}

		PatchLog("[Patch] hooks installed: Referee x4, TLS, SendRequest, CheatMgr, DUID, ImgSig, getaddrinfo, GetAddrInfoW%s%s",
			photonConnectHook ? ", PhotonNsPort" : "", RR::Config::EnableTracing ? " + tracing" : "");
		// Either host may legitimately be unset -- both come from the ini and nothing is compiled in --
		// so say so rather than logging a blank.
		const char* apiHost = *RR::Config::ApiHost ? RR::Config::ApiHost : "(unchanged -- no ApiHost set)";
		if (!*RR::Config::PhotonHost) {
			PatchLog("[Patch] API host=%s  Photon=(unchanged -- no PhotonHost set)", apiHost);
		}
		else if (RR::Config::PhotonPort) {
			PatchLog("[Patch] API host=%s  Photon=%s  port=%d",
				apiHost, RR::Config::PhotonHost, RR::Config::PhotonPort);
		}
		else {
			PatchLog("[Patch] API host=%s  Photon=%s  port=(as supplied)",
				apiHost, RR::Config::PhotonHost);
		}
	}
}
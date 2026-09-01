#pragma once
#include "../Utils/globals.h"

namespace RR::Methods::Referee {
	uintptr_t Check1 = 0x76E850;
	uintptr_t Check2 = 0x2E6F680;
	uintptr_t Check3 = 0x2E6F670;
	uintptr_t Check4 = 0x2E6F6E0;
}

namespace RR::Methods::LegacyTlsAuthentication {
	uintptr_t NotifyServerCertificate = 0x77E5E30;
}

namespace RR::Methods::HTTPRequest {
	uintptr_t SendRequest = 0x77E0950;
	// BestHTTP.HTTPRequest.CallCallback() -- runs once a response has arrived, so it is the one place
	// where the request AND its response are both in hand. Needed to correlate URL -> status -> body:
	// a deserialize failure only tells you the expected TYPE, never which endpoint produced the bytes.
	uintptr_t CallCallback = 0x77EBAE0;
}

// Photon.Realtime.AppSettings -- the settings object Photon's ConnectUsingSettings reads. Field
// offsets from the runtime dump (build 19/07/25); these names are NOT obfuscated.
//
// NOTHING HERE IS EVER READ BY THIS CLIENT. Kept only as the map of an object that looks like the
// obvious seam and is not one -- read the DEAD SEAM block below before reaching for it again.
namespace RR::Offsets::AppSettings {
	constexpr int AppIdRealtime  = 16;   // Il2CppString*
	constexpr int AppIdChat      = 24;   // Il2CppString*
	constexpr int AppIdVoice     = 32;   // Il2CppString*
	constexpr int AppVersion     = 40;   // Il2CppString*
	constexpr int UseNameServer  = 48;   // bool
	constexpr int FixedRegion    = 56;   // Il2CppString*
	constexpr int Server         = 72;   // Il2CppString*
	constexpr int Port           = 80;   // int32
}

// =============================================================================================
// DEAD SEAM: OLPEILEPEAD.JBNCMFDFDLM(AppSettings) at 0x757E8C0 -- Photon's ConnectUsingSettings.
//
// It IS the right method. Disassembly shows it copying AppIdRealtime/AppVersion/UseNameServer onto
// the LoadBalancingClient, and on the name-server branch doing exactly
//     mov eax, [rbx+0x50]      ; appSettings.Port
//     mov [rdi+0x180], eax     ; this.NameServerPortInAppSettings
// so writing AppSettings.Port there would have worked.
//
// THIS CLIENT NEVER CALLS IT. The hook installed fine -- MinHook's E9 is visible at 0x757E8C0 in a
// live image capture -- and its body never ran once: not one [Photon] Port line exists in any
// session log, including sessions where the getaddrinfo redirect logged normally on the very same
// connect. callxref and mxref both find zero references to it. Rec Room configures the
// LoadBalancingClient field by field and connects through ConnectToNameServer instead.
//
// That also corrects an older finding recorded in Patches.h: the AppSettings.AppId* writes did not
// show that "the client ignores those fields", they showed the hook never ran. Do not re-hook this
// address, and do not read a silent AppSettings write as evidence about what the client consumes.
// =============================================================================================

// Photon.Realtime.LoadBalancingClient (OLPEILEPEAD) -- the connection object itself.
namespace RR::Offsets::LoadBalancingClient {
	// NameServerPortInAppSettings. GetNameServerAddress formats "<NameServerHost>:<port>", and this
	// field wins over the protocol default whenever it is non-zero:
	//     cmp dword ptr [rbx+0x180], 0 / jne -> use [rbx+0x180]
	// The defaults it overrides are 5058 (UDP) and 27000 (alternative UDP ports). It is the only
	// writable port input on the name-server connect, which is why PhotonPort is applied here.
	constexpr int NameServerPortInAppSettings = 0x180;   // int32
}

// OLPEILEPEAD.FHFJBMBEBPP() -- GetNameServerAddress, and the live seam for the connect port. Every
// path that reaches the name server (ConnectToNameServer, ConnectToRegionMaster, the
// NameServerAddress getter) funnels through it, and it reads NameServerPortInAppSettings on the
// way -- so writing that field just before the original runs is what makes PhotonPort take effect.
// The host itself is still swapped at the DNS layer. Only hooked when PhotonHost and PhotonPort are
// both set.
namespace RR::Methods::Photon {
	uintptr_t GetNameServerAddress = 0x757B960;
}

// Photon transport encryption.
//
// Luxon implements ONLY Photon's PayloadEncryption (it has Encrypt/DecryptPayload, and never reads
// the EncryptionMode=193 parameter the client sends). The 2025 client negotiates DATAGRAM
// encryption, so after the handshake it encrypts the whole ENet datagram -- Luxon then fails CRC /
// protocol validation and drops every packet SILENTLY (those catch blocks only bump a metric).
// Symptom: "Established encryption" followed by ~43s of nothing, client retrying every 5.03s until
// it throws "Unable to send message!". The 2023 client uses payload encryption and works fine.
//
// EnetPeer.IsTransportEncrypted() is the gate the send path consults; forcing it false keeps the
// client on payload encryption, which is exactly what Luxon speaks.
namespace RR::Methods::Photon {
	uintptr_t EnetPeer_IsTransportEncrypted = 0x7521380;
}

// RecNet's System.Net.Http-based client (CBACIMLIBPF). The room-save/asset downloads go through
// THIS, not BestHTTP, so HTTPManager.SendRequest never sees them -- which is why a room join logs
// ~97 requests and not one blob fetch. Both overloads are static; the request path is the 3rd
// argument (r8). Hooking them makes the save-data download visible.
namespace RR::Methods::HttpClient {
	uintptr_t Request9  = 0x7CC50D0;  // IBJKIAMJCDN(HttpMethod, service, path, ...) 9 params
	uintptr_t Request10 = 0x7CC4F20;  // IBJKIAMJCDN(HttpMethod, service, path, ..., Queue) 10 params

	// The two stages BELOW the enqueue, for locating where a request dies.
	//
	// CBACIMLIBPF dispatches in three steps: IBJKIAMJCDN puts the request on a shared Queue,
	// KOMEOKGFOBP drains that queue, and CJOCHJBHGPM performs one actual send (its uint first arg is
	// the attempt number, so retries are visible). Only after CJOCHJBHGPM does it become a real
	// BestHTTP request and show up in our SendRequest hook.
	//
	// This matters because the room-save blob is logged by the Request9/10 hook and then never
	// appears in SendRequest -- and we PROVED it is not slipping out via some other transport: every
	// one of the 177 [HTTP<-] responses in the 2026-08-17 run matched a SendRequest, zero unmatched.
	// So it dies inside these two. Pump reached but not Send => stuck in the queue. Send reached but
	// no SendRequest => dying below the send.
	uintptr_t Pump = 0x7CC6EA0;  // KOMEOKGFOBP(Queue<CPPAPPOPHFD>, CancellationToken)
	uintptr_t Send = 0x7CC29E0;  // CJOCHJBHGPM(uint attempt, HttpMethod, service, path, ...)
}

// Field offsets from the runtime dump (build 19/07/25), for the response logger.
namespace RR::Offsets {
	// BestHTTP.HTTPRequest
	constexpr int Request_Uri        = 16;   // System.Uri*
	constexpr int Request_Response   = 112;  // BestHTTP.HTTPResponse*
	// BestHTTP.HTTPResponse
	constexpr int Response_StatusCode = 24;  // int32
	constexpr int Response_Data       = 72;  // byte[]*
	constexpr int Response_DataLength = 80;  // int32
	constexpr int Response_DataAsText = 96;  // Il2CppString* (populated once the client reads it)
	// il2cpp array: klass(8) + monitor(8) + bounds(8) + max_length(8), payload follows
	constexpr int Array_Data = 32;
}

// Image content-signature verification (HPJKKCCECLH, RecNet.Runtime).
//
// img.recflare.net serves ARCHIVED rec.net assets verbatim, so every image still carries the
// original response header
//     content-signature: key-id=KEY:RSA:p1.rec.net; data=<base64 RSA signature>
// which is what the `?sig=p1` query string selects. The client verifies that signature against an
// RSA public key baked into OGONMIPNEKK() and, on mismatch, throws MELLIJAOIBB "Image signature
// verification failed" out of the async OIKINEOLHBL(url, bool, ct). The bytes served are a valid
// JPEG (FF D8 FF DB) -- it is the signature that cannot be satisfied, because reproducing it would
// need Rec Room's private key. For an archival client the check is unsatisfiable by construction.
//
// Verify is `private static bool BLDFMLMAFLH(byte[] data, byte[] signature)`: a static il2cpp
// method, so the two arrays arrive in RCX/RDX with MethodInfo* in R8. Forcing it true is the fix.
namespace RR::Methods::ImageSignature {
	uintptr_t Verify    = 0x7CF32F0;  // BLDFMLMAFLH(byte[] data, byte[] sig) -> bool
	uintptr_t PublicKey = 0x7CF84E0;  // OGONMIPNEKK() -> RSAParameters   (not hooked; for reference)
}

// AppExitState setter -- PGEBFPENFNN.set_MNMHGMIGNCP(MJNGPKFGDEA) in Assembly-CSharp.
//
// This is the method that emits "AppExitState changed: X => Y" (the dump still carries the
// interpolated string in its body, which is how it was identified despite the obfuscated name).
// Enum: 0=Running, 1=PreparingToExit, 2=ReadyForExit.
//
// Hooked purely to answer "who asked the client to quit". The client exits CLEANLY -- its own
// telemetry reports app_exit_state=ReadyForExit, crash_detected=false -- so it is not a crash;
// something calls this. A native stack capture at the transition names the caller, which beats
// inferring it from whatever error happened to be logged nearby (an NRE fires ~0.65s before, but the
// same NRE also fires repeatedly WITHOUT an exit, so proximity alone proves nothing).
namespace RR::Methods::AppLifecycle {
	uintptr_t SetExitState = 0x21498B0;

	// UnityEngine.Application.Quit() and Quit(int). RVAs from il2cpp-2025/methods.pkl -- UnityEngine
	// is NOT in the Cpp2IL dump, but the live-carve index covers it (346,938 methods vs 241,704).
	// Hooked to distinguish a managed quit request from an OS window-close; see AppQuit_H.
	uintptr_t ApplicationQuit  = 0x99584D0;
	uintptr_t ApplicationQuit2 = 0x9958510;
	// For reference, not hooked: Internal_ApplicationWantsToQuit = 0x99581E0 (where the exit stack
	// enters managed code), Internal_ApplicationQuit = 0x9958140.

	// Rec Room's own shutdown API. This class is UNOBFUSCATED, which is a gift -- the stack from the
	// Application.Quit hook landed in its async helper NFIGOBHOJIF(int) @0x2174B00, so the quit is
	// requested by game logic, and one of these entry points carries the reason.
	// FatalApplicationQuit takes an int code AND a message string; that message is the whole answer.
	// LogoutToBootScene* matter because the observed symptom is "it auto logs out, then quits".
	uintptr_t TryApplicationQuit0    = 0x2177A00;  // TryApplicationQuit()
	uintptr_t TryApplicationQuit1    = 0x2177880;  // TryApplicationQuit(int)
	uintptr_t FatalApplicationQuit   = 0x2170A70;  // FatalApplicationQuit(int, string)
	uintptr_t LogoutToBootScene      = 0x2174650;  // LogoutToBootScene()
	uintptr_t LogoutToBootSceneAsync = 0x2174590;  // LogoutToBootSceneAsync()
}

// ANTI-CHEAT MODULE SCAN -- the reason the client "auto logs out and quits" while sitting idle.
//
// CheatManager (Assembly-CSharp, class name NOT obfuscated) enumerates loaded modules, strips the
// app directory from each path (its sibling closure JFOAEHOCCIN.EAMAJENBDLL does `s.Replace(appPath,
// "")`), formats the survivors as "[index:name]", and hands them to this callback:
//
//     private sealed class LFKHKEPJKMB {          // compiler-generated closure
//         public string filenames;                //  field @0x10
//         internal void LOELLDBOINB() {
//             SessionManager.FatalApplicationQuit(533223478, filenames);
//         }
//     }
//
// Confirmed at runtime 2026-08-17: it fired with filenames = "[0:2025Patch.dll]" -- our own injected
// DLL -- 30ms before AppExitState went to PreparingToExit. That is why the quit looked orderly, why
// no server-side fix ever touched it, and why Photon/logout/RaiseEvent were all downstream noise.
//
// 533223478 (0x1FC85836) is a fixed constant baked into LOELLDBOINB, not a dynamic reason code; it
// ends up as the process exit code via Application.Quit(int). Searching the binary for that immediate
// is what located this call site, and it is the fastest way to re-find it after a build rolls.
namespace RR::Methods::AntiCheat {
	uintptr_t ModuleScanDetected = 0x2148FB0;  // CheatManager+LFKHKEPJKMB.LOELLDBOINB()
}

namespace RR::Methods::System::Uri {
	uintptr_t ToString = 0x935AF50;
	uintptr_t ctor = 0x935C2A0;
}

namespace RR::Methods::Il2cpp {
	uintptr_t il2cpp_string_new;
	uintptr_t il2cpp_object_get_class;
	uintptr_t il2cpp_object_new;
	// Runs a class's static constructor if it has not run yet -- the same thing the generated code
	// does inline (`cmp dword ptr [klass+0xE8], 0` / call the init thunk) before touching statics.
	// Needed because we write a static field of DMIBBCKIGCG, and its .cctor is what puts the stock
	// value there: writing first and letting the cctor run afterwards would just undo us.
	uintptr_t il2cpp_runtime_class_init;
}

// TACHYON VOICE -- the RSA public key the voice handshake is encrypted to.
//
// TachyonClient (BAOFAOBLAMJ) hands the voice server a JSON blob {AI, AT, VB, CKA, CIA, CPK}: AT is
// the Photon auth payload AES-encrypted, and CKA/CIA are that AES key and IV RSA-encrypted to a
// public key baked into the client. A self-hosted voice server therefore cannot read ANY of it --
// decrypting CKA/CIA needs Rec Room's private key, which is what makes AT unauthenticatable.
//
// The key enters at exactly one place. BAOFAOBLAMJ..ctor:
//     0x82796A3  new RSACryptoServiceProvider()          -> this.KODJDHFMKAC (+0x40)
//     0x8279726  rdx = DMIBBCKIGCG::statics[+0x08]       -> the <RSAKeyValue> XML string
//     0x827972A  call [klass+0x200]                      -> rsa.FromXmlString(rdx)  (virtual)
// and the XML literal (0xD106B70, 243 chars) is referenced ONLY by DMIBBCKIGCG..cctor (0x827A820),
// which stores it to that static. So swapping the static before the ctor reads it swaps the key for
// the whole process -- and the game performs the FromXmlString itself, so we never have to make a
// managed call that could throw back through a spoofed return address.
//
// ⚠️ DMIBBCKIGCG.NDEDJKDIGGM()/CKAAEJMIMEF() LOOK like the getters for these statics but have zero
// callers -- the ctor reads the field directly. Do not hook them.
namespace RR::Methods::Tachyon {
	uintptr_t Ctor = 0x82795E0;  // BAOFAOBLAMJ..ctor(BAAIILIKHPH)
}

namespace RR::Offsets::Tachyon {
	// Metadata-usage slot holding Il2CppClass* for DMIBBCKIGCG (the key/constant holder). Taken from
	// the ctor's own `mov rax, [rip+0x4F303E6]` at 0x827970B, so it is the same pointer the game uses.
	constexpr uintptr_t KeyHolderTypeInfo  = 0xD1A9AF8;
	constexpr int       Class_StaticFields = 0x90;  // Il2CppClass -> static field block
	constexpr int       Static_ServerKeyXml = 0x08; // DMIBBCKIGCG.KBANIKNNKKD (the <RSAKeyValue> XML)
}
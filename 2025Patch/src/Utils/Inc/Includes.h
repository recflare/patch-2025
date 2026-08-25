#pragma once
#define WIN32_LEAN_AND_MEAN // keeps Windows.h from pulling in old winsock.h, which clashes with winsock2/ws2tcpip
#include <Windows.h>
#include <iostream>
#include <cstdint>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include <new>
#include <string>

// File logger, in addition to the AllocConsole stdout. The console window steals focus from the game
// (Unity pauses when unfocused) and can't be captured after the fact; a flushed log file survives a
// crash and can be read on any machine. Writes next to the game exe as 2025patch.log -- or, for a
// second client running side by side, 2025patch.<pid>.log (see below).
inline void PatchLog(const char* fmt, ...) {
	static FILE* lf = nullptr;
	if (!lf) {
		char path[MAX_PATH];
		GetModuleFileNameA(nullptr, path, MAX_PATH);      // the game exe (Recroom_Release.exe)
		char* slash = strrchr(path, '\\');
		if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "2025patch.log");
		// _SH_DENYWR so a SECOND patched client can't open this same file for writing: both would
		// truncate it and then write at their own independent offsets, shredding the log of whichever
		// got there first. The one that loses falls back to 2025patch.<pid>.log, so the plain name
		// always belongs to the first instance -- the one every other note here tells you to read --
		// and only writing is denied, so tailing or opening it in an editor still works.
		lf = _fsopen(path, "w", _SH_DENYWR);
		if (!lf) {
			char* dot = strrchr(path, '.');
			if (dot) sprintf_s(dot, sizeof(path) - (dot - path), ".%lu.log", GetCurrentProcessId());
			lf = _fsopen(path, "w", _SH_DENYWR);
		}
		if (!lf) return;
	}
	// Timestamp every line. Unity's Player.log is timestamped, and the interesting failures span both
	// files (e.g. a Photon disconnect here vs. "Unable to send message!" there) -- without a clock on
	// these lines the two logs cannot be correlated.
	SYSTEMTIME st; GetLocalTime(&st);
	fprintf(lf, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	va_list ap; va_start(ap, fmt);
	vfprintf(lf, fmt, ap);
	va_end(ap);
	fputc('\n', lf);
	fflush(lf);   // flush every line so nothing is lost if the process is killed mid-boot
}


#include "../deps/minhook/include/MinHook.h"

struct Il2cppString {
	int Length;
	wchar_t s[1024];
};
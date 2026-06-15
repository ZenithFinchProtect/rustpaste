#pragma once

#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <codecvt>
#include "mutex.hpp"

#include "xorstr.hpp"
#include "communication_structures.hpp"

using namespace structures;
class Vector3;

#define alternative(a, b) std::holds_alternative<a>(b)

// WDA_EXCLUDEFROMCAPTURE was added in Windows 10 2004 (build 19041);
// older SDK headers may not define it. WDA_MONITOR is the legacy fallback.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

inline std::string utf16_to_utf8 ( std::u16string utf16_string )
{
	std::wstring_convert<std::codecvt_utf8_utf16<int16_t>, int16_t> convert;
	auto p = reinterpret_cast<const int16_t*>( utf16_string.data ( ) );
	return convert.to_bytes ( p, p + utf16_string.size ( ) );
}

//
// Usermode replacement for the original kernel-driver / shared-memory comms class.
// Drop-in: same public API, same call sites in main.cpp / object_loop.cpp etc.
// Uses OpenProcess + ReadProcessMemory / WriteProcessMemory under the hood.
// Intended for non-EAC servers where no anti-cheat is running.
//
class CSharedMemComms
{
	bool                Initialized = false;
	int					iCounter = 0;
	bool				bMutex;

	// Shared across all comms instances - attach() opens once for the whole process.
	static inline HANDLE s_hProcess = nullptr;
	static inline DWORD  s_pid = 0;

	// Replicates ricochet::crypto::hash from crypto.hpp at runtime so that the
	// compile-time h("RustClient.exe") / h("UnityPlayer.dll") hashes match.
	static unsigned int runtime_hash ( const char* str )
	{
		auto hash = 0xffffffffu;
		while ( *str != '\0' )
		{
			unsigned char c = static_cast<unsigned char>( *str++ );
			// Case-insensitive: lowercase before hashing
			if ( c >= 'A' && c <= 'Z' ) c += 32;
			hash ^= c;
			hash ^= 0x4447bbeeu;
			hash = ( hash >> 1 );
			hash ^= 0x092cd4afu;
			hash += ( hash << 5 );
		}
		return hash;
	}

	static unsigned int runtime_hash_ci ( const wchar_t* wstr )
	{
		char narrow[MAX_PATH] { };
		WideCharToMultiByte ( CP_ACP, 0, wstr, -1, narrow, MAX_PATH, nullptr, nullptr );
		return runtime_hash ( narrow );
	}

public:

	// Kept as a public dummy to preserve the legacy public field for any external
	// references; nothing in this codebase actually dereferences it after the rewrite.
	void*               pSharedMemData = nullptr;
	br::mutex guard;
	std::chrono::steady_clock::time_point start;

	CSharedMemComms ( LPCSTR lpName, bool use_mutex ) : bMutex { use_mutex }
	{
		// lpName intentionally ignored - no shared memory in usermode mode.
		(void)lpName;
#ifdef _DEBUG
		printf ( "[comms] usermode stub constructed (was: %s)\n", lpName ? lpName : "<null>" );
#endif
	}

	~CSharedMemComms ( )
	{
		// Per-instance: nothing to release. Process handle is shared and freed once
		// on detach() (or by the OS at process exit).
	}

	void Initialize ( )
	{
		// Driver-wait removed: there's no driver to sync with. Just mark ready.
		Initialized = true;
		start = std::chrono::steady_clock::now ( );
	}

	template <typename t>
	t read ( const uint64_t address )
	{
		if ( address < 0xffffff || address > 0x7fffffff0000 )
			return t ( );

		if ( !s_hProcess )
			return t ( );

		t buffer { };
		SIZE_T bytes_read = 0;
		if ( !ReadProcessMemory ( s_hProcess, reinterpret_cast<LPCVOID>( address ), &buffer, sizeof ( t ), &bytes_read )
			|| bytes_read != sizeof ( t ) )
		{
			return t ( );
		}
#ifdef _DEBUG
		iCounter++;
#endif
		return buffer;
	}

	template <typename t>
	bool read ( const uintptr_t address, const t buffer, const size_t size )
	{
		if ( address < 0xffffff || address > 0x7fffffff0000 )
			return false;

		if ( !s_hProcess )
			return false;

		SIZE_T bytes_read = 0;
		const BOOL ok = ReadProcessMemory ( s_hProcess,
			reinterpret_cast<LPCVOID>( address ),
			reinterpret_cast<LPVOID>( buffer ),
			size,
			&bytes_read );
#ifdef _DEBUG
		iCounter++;
#endif
		return ok && bytes_read == size;
	}

	template <typename t>
	bool write ( const uintptr_t address, t buffer )
	{
		if ( address < 0xffffff || address > 0x7fffffff0000 )
			return false;

		if ( !s_hProcess )
			return false;

		SIZE_T bytes_written = 0;
		const BOOL ok = WriteProcessMemory ( s_hProcess,
			reinterpret_cast<LPVOID>( address ),
			&buffer,
			sizeof ( t ),
			&bytes_written );
#ifdef _DEBUG
		iCounter++;
#endif
		return ok && bytes_written == sizeof ( t );
	}

	template <typename t>
	bool write ( const uintptr_t address, const t buffer, const size_t size )
	{
		if ( address < 0xffffff || address > 0x7fffffff0000 )
			return false;

		if ( !s_hProcess )
			return false;

		SIZE_T bytes_written = 0;
		const BOOL ok = WriteProcessMemory ( s_hProcess,
			reinterpret_cast<LPVOID>( address ),
			reinterpret_cast<LPCVOID>( buffer ),
			size,
			&bytes_written );
#ifdef _DEBUG
		iCounter++;
#endif
		return ok && bytes_written == size;
	}

	bool attach ( const uint32_t hash )
	{
		const br::locker noob ( this->guard );

		if ( s_hProcess )
		{
			printf ( "[comms] attach: already attached to pid %lu (handle %p)\n", s_pid, s_hProcess );
			return true;
		}

		printf ( "[comms] attach: looking for process hash 0x%08X\n", hash );

		HANDLE snap = CreateToolhelp32Snapshot ( TH32CS_SNAPPROCESS, 0 );
		if ( snap == INVALID_HANDLE_VALUE )
		{
			printf ( "[comms] attach: CreateToolhelp32Snapshot failed (err %lu)\n", GetLastError ( ) );
			return false;
		}

		PROCESSENTRY32W pe { };
		pe.dwSize = sizeof ( pe );
		bool found = false;
		int  scanned = 0;

		if ( Process32FirstW ( snap, &pe ) )
		{
			do
			{
				++scanned;
				char narrow[MAX_PATH] { };
				WideCharToMultiByte ( CP_ACP, 0, pe.szExeFile, -1, narrow, MAX_PATH, nullptr, nullptr );
				const unsigned int this_hash = runtime_hash ( narrow );
				if ( this_hash == hash )
				{
					s_pid = pe.th32ProcessID;
					found = true;
					printf ( "[comms] attach: matched '%s' -> pid %lu (hash 0x%08X)\n", narrow, s_pid, this_hash );
					break;
				}
			}
			while ( Process32NextW ( snap, &pe ) );
		}
		CloseHandle ( snap );

		if ( !found )
		{
			// Fallback: Rust may append trailing Unicode chars to the exe name
			// (anti-cheat obfuscation). Try case-insensitive prefix match.
			HANDLE snap2 = CreateToolhelp32Snapshot ( TH32CS_SNAPPROCESS, 0 );
			if ( snap2 != INVALID_HANDLE_VALUE )
			{
				PROCESSENTRY32W pe2 { };
				pe2.dwSize = sizeof ( pe2 );
				if ( Process32FirstW ( snap2, &pe2 ) )
				{
					do
					{
						char narrow2[MAX_PATH] { };
						WideCharToMultiByte ( CP_ACP, 0, pe2.szExeFile, -1, narrow2, MAX_PATH, nullptr, nullptr );
						// Check if name starts with "rustclient.exe" (case-insensitive)
						if ( _strnicmp ( narrow2, "rustclient.exe", 14 ) == 0 )
						{
							s_pid = pe2.th32ProcessID;
							found = true;
							printf ( "[comms] attach: prefix-matched '%s' -> pid %lu\n", narrow2, s_pid );
							break;
						}
					}
					while ( Process32NextW ( snap2, &pe2 ) );
				}
				CloseHandle ( snap2 );
			}
		}

		if ( !found )
		{
			printf ( "[comms] attach: hash 0x%08X NOT found after scanning %d processes\n", hash, scanned );
			printf ( "[comms] attach: is RustClient.exe actually running? (or is the h() string wrong?)\n" );
			return false;
		}

		s_hProcess = OpenProcess (
			PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
			FALSE,
			s_pid );

		if ( !s_hProcess )
		{
			const DWORD err = GetLastError ( );
			printf ( "[comms] attach: OpenProcess(pid %lu) failed: %lu", s_pid, err );
			if ( err == 5 )
				printf ( "  (ACCESS_DENIED -- run the .exe AS ADMINISTRATOR)" );
			else if ( err == 87 )
				printf ( "  (INVALID_PARAMETER)" );
			else if ( err == 6 )
				printf ( "  (INVALID_HANDLE)" );
			printf ( "\n" );
		}
		else
		{
			printf ( "[comms] attach: OpenProcess(pid %lu) OK (handle %p)\n", s_pid, s_hProcess );
		}

		return s_hProcess != nullptr;
	}

	bool detach ( )
	{
		const br::locker noob ( this->guard );

		if ( s_hProcess )
		{
			CloseHandle ( s_hProcess );
			s_hProcess = nullptr;
			s_pid = 0;
		}
		return true;
	}

	uintptr_t find ( const uint32_t hash )
	{
		const br::locker noob ( this->guard );

		if ( !s_pid )
		{
			printf ( "[comms] find: not attached (s_pid == 0) - call attach() first\n" );
			return 0;
		}

		printf ( "[comms] find: looking for module hash 0x%08X in pid %lu\n", hash, s_pid );

		// Loop briefly in case the target module hasn't been loaded yet.
		for ( int retry = 0; retry < 20; ++retry )
		{
			HANDLE snap = CreateToolhelp32Snapshot ( TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, s_pid );
			if ( snap == INVALID_HANDLE_VALUE )
			{
				const DWORD err = GetLastError ( );
				if ( retry == 0 )
					printf ( "[comms] find: CreateToolhelp32Snapshot failed (err %lu) - will retry\n", err );
				Sleep ( 250 );
				continue;
			}

			MODULEENTRY32W me { };
			me.dwSize = sizeof ( me );
			uintptr_t result = 0;
			int       scanned = 0;

			if ( Module32FirstW ( snap, &me ) )
			{
				do
				{
					++scanned;
					char narrow[MAX_PATH] { };
					WideCharToMultiByte ( CP_ACP, 0, me.szModule, -1, narrow, MAX_PATH, nullptr, nullptr );
					if ( runtime_hash ( narrow ) == hash )
					{
						result = reinterpret_cast<uintptr_t>( me.modBaseAddr );
						printf ( "[comms] find: matched '%s' -> base 0x%llX (after %d modules)\n",
							narrow, (unsigned long long)result, scanned );
						break;
					}
					// Fallback: prefix match for known modules (trailing Unicode chars)
					if ( !result )
					{
						if ( ( _strnicmp ( narrow, "gameassembly.dll", 16 ) == 0 && _strnicmp ( "gameassembly.dll", "gameassembly.dll", 16 ) == 0 )
							|| _strnicmp ( narrow, "unityplayer.dll", 15 ) == 0
							|| _strnicmp ( narrow, "rustclient.exe", 14 ) == 0 )
						{
							unsigned int prefix_hash = runtime_hash_ci ( me.szModule );
							// Only accept if the caller is looking for this specific module
							// We can't tell which, so just try the prefix hash
							if ( prefix_hash != hash )
							{
								// Compute hash of just the clean prefix
								const char* known_names[] = { "gameassembly.dll", "unityplayer.dll", "rustclient.exe" };
								for ( auto kn : known_names )
								{
									if ( _strnicmp ( narrow, kn, strlen ( kn ) ) == 0 && runtime_hash ( kn ) == hash )
									{
										result = reinterpret_cast<uintptr_t>( me.modBaseAddr );
										printf ( "[comms] find: prefix-matched '%s' as '%s' -> base 0x%llX\n",
											narrow, kn, (unsigned long long)result );
										break;
									}
								}
								if ( result ) break;
							}
						}
					}
				}
				while ( Module32NextW ( snap, &me ) );
			}
			CloseHandle ( snap );

			if ( result )
				return result;

			if ( retry == 0 )
				printf ( "[comms] find: hash 0x%08X not in %d modules yet - retrying for ~5s\n", hash, scanned );
			Sleep ( 250 );
		}
		printf ( "[comms] find: hash 0x%08X NOT found after retry loop\n", hash );
		return 0;
	}

	bool get_key ( const int key )
	{
		return ( GetAsyncKeyState ( key ) & 0x8000 ) != 0;
	}

	bool hide_overlay ( uintptr_t window_handle )
	{
		// SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) hides the window from
		// screen-capture APIs - closest usermode equivalent to the driver's hide_window.
		return SetWindowDisplayAffinity ( reinterpret_cast<HWND>( window_handle ), WDA_EXCLUDEFROMCAPTURE ) != FALSE;
	}

	// External-mode fallback for bone position. The original driver attached to
	// the game thread and called Unity's Transform.get_position() to compute the
	// world-space position from the transform hierarchy. Without the driver we
	// just read whatever vector lives at the given address. If bones look wrong
	// (e.g. all at world origin or local space) a proper Unity transform walker
	// will need to be implemented here.
	template <typename t>
	t get_position_injected ( const uintptr_t transform )
	{
		if ( transform < 0xffffff || transform > 0x7fffffff0000 )
			return t ( );

		if ( !s_hProcess )
			return t ( );

		t buffer { };
		SIZE_T bytes_read = 0;
		if ( !ReadProcessMemory ( s_hProcess, reinterpret_cast<LPCVOID>( transform ), &buffer, sizeof ( t ), &bytes_read )
			|| bytes_read != sizeof ( t ) )
		{
			return t ( );
		}
		return buffer;
	}

	std::string read_ascii ( const uintptr_t address, const size_t size )
	{
		std::unique_ptr<char[]> buffer ( new char[size] );
		if ( !read<char*> ( address, buffer.get ( ), size ) )
			return std::string ( );
		return std::string ( buffer.get ( ) );
	}

	std::string read_unicode ( const uintptr_t address, const int stringLength )
	{
		char16_t wcharTemp[128] = { '\0' };

		if ( !read<char16_t*> ( address, wcharTemp, stringLength * 2 ) )
			return std::string ( "" );

		std::string u8_conv = utf16_to_utf8 ( wcharTemp );

		return u8_conv;
	}

	std::wstring wread_unicode ( const uintptr_t address, const int stringLength )
	{
		char16_t wcharTemp[128] = { '\0' };

		if ( !read<char16_t*> ( address, wcharTemp, stringLength * 2 ) )
			return std::wstring ( L"" );

		return std::wstring ( reinterpret_cast<wchar_t*>( wcharTemp ) );
	}
};

namespace mist
{
	extern std::unique_ptr< CSharedMemComms > esp;
	extern std::unique_ptr< CSharedMemComms > aim;
	extern std::unique_ptr< CSharedMemComms > obj;
	extern std::unique_ptr< CSharedMemComms > misc;
	extern std::unique_ptr< CSharedMemComms > other;
	extern std::unique_ptr< CSharedMemComms > wep;
};

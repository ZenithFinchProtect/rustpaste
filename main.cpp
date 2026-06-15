#include "include.hpp"
#include "math.hpp"
#include "object_loop.hpp"
#include "esp.hpp"
#include "aimbot.hpp"
#include "misc.hpp"
#include <DbgHelp.h>
#include <ShlObj_core.h>

#include "timer.hpp"

#pragma comment( lib, "ws2_32.lib" )

static void pause_console ( const char* reason )
{
	printf ( "\n\u001b[33m[!] %s\u001b[0m\n", reason );
	printf ( "\u001b[37mPress ENTER to close...\u001b[0m\n" );
	fflush ( stdout );
	(void)getchar ( );
}

LONG WINAPI ExpHandler ( PEXCEPTION_POINTERS ExceptionInfo )
{
	MINIDUMP_EXCEPTION_INFORMATION M;

	M.ThreadId = GetCurrentThreadId ( );
	M.ExceptionPointers = ExceptionInfo;
	M.ClientPointers = 0;

	const DWORD code = ExceptionInfo && ExceptionInfo->ExceptionRecord ? ExceptionInfo->ExceptionRecord->ExceptionCode : 0;
	const void* addr = ExceptionInfo && ExceptionInfo->ExceptionRecord ? ExceptionInfo->ExceptionRecord->ExceptionAddress : nullptr;
	logging::print ( TYPE_ERROR, "Unhandled exception 0x%08X at %p", code, addr );

	mist::other.release ( );
	mist::aim.release ( );
	mist::esp.release ( );
	mist::misc.release ( );
	mist::obj.release ( );
	mist::wep.release ( );

	HANDLE hFile = CreateFileA ( _("C:\\CATJPG\\crash.dmp"), GENERIC_READ | GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0 );
	if ( hFile != INVALID_HANDLE_VALUE )
	{
		MiniDumpWriteDump ( GetCurrentProcess ( ), GetCurrentProcessId ( ), hFile, MiniDumpNormal, &M, NULL, NULL );
		CloseHandle ( hFile );
		logging::print ( TYPE_LOG, "Crash dump written to C:\\CATJPG\\crash.dmp" );
	}

	pause_console ( "Cheat crashed." );
	return EXCEPTION_EXECUTE_HANDLER;
}

void OnDetach ( )
{
	mist::other.release ( );
	mist::aim.release ( );
	mist::esp.release ( );
	mist::misc.release ( );
	mist::obj.release ( );
	mist::wep.release ( );
}

static void ensure_console ( )
{
	// Always allocate a console so debug prints are visible in release builds too.
	if ( !GetConsoleWindow ( ) )
		AllocConsole ( );

	FILE* f = nullptr;
	freopen_s ( &f, "CONOUT$", "w", stdout );
	freopen_s ( &f, "CONOUT$", "w", stderr );
	freopen_s ( &f, "CONIN$", "r", stdin );

	// Enable ANSI colour escape codes on Windows 10+.
	HANDLE h = GetStdHandle ( STD_OUTPUT_HANDLE );
	DWORD mode = 0;
	if ( GetConsoleMode ( h, &mode ) )
		SetConsoleMode ( h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */ );

	SetConsoleTitleA ( "ricochet :: debug console" );
}

static bool running_as_admin ( )
{
	BOOL isAdmin = FALSE;
	PSID adminGroup = nullptr;
	SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
	if ( AllocateAndInitializeSid ( &ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup ) )
	{
		CheckTokenMembership ( nullptr, adminGroup, &isAdmin );
		FreeSid ( adminGroup );
	}
	return isAdmin == TRUE;
}

#ifdef _WINDLL
DWORD WINAPI MainThread ( PVOID )
#else
int main ( )
#endif
{
	ensure_console ( );
	logging::print ( TYPE_LOG, "ricochet starting..." );

	if ( !running_as_admin ( ) )
	{
		logging::print ( TYPE_WARNING, "NOT running as administrator." );
		logging::print ( TYPE_WARNING, "OpenProcess against RustClient.exe will likely fail with ERROR_ACCESS_DENIED (5)." );
		logging::print ( TYPE_WARNING, "Right-click the .exe and 'Run as administrator'." );
	}
	else
	{
		logging::print ( TYPE_DEBUG, "Running as administrator. OK." );
	}

	logging::print ( TYPE_DEBUG, "Setting exception handlers.." );
	SetUnhandledExceptionFilter ( ExpHandler );
	std::atexit ( OnDetach );

	logging::print ( TYPE_DEBUG, "Creating comms instances.." );
	mist::other = std::make_unique< CSharedMemComms > ( _ ( "Global\\7165353" ), true );
	mist::aim = std::make_unique< CSharedMemComms > ( _ ( "Global\\71696969" ), false );
	mist::esp = std::make_unique< CSharedMemComms > ( _ ( "Global\\742257" ), false );
	mist::misc = std::make_unique< CSharedMemComms > ( _ ( "Global\\1574441" ), true );
	mist::obj = std::make_unique < CSharedMemComms > ( _ ( "Global\\7545711111" ), true );
	mist::wep = std::make_unique < CSharedMemComms > ( _ ( "Global\\15646577877" ), true );

	logging::print ( TYPE_DEBUG, "Initializing comms.." );
	mist::other->Initialize ( );
	mist::aim->Initialize ( );
	mist::esp->Initialize ( );
	mist::misc->Initialize ( );
	mist::obj->Initialize ( );
	mist::wep->Initialize ( );
	Beep ( 512, 256 );
	logging::print ( TYPE_DEBUG, "Cheat loaded. Waiting for game window.." );

	std::thread overlay ( esp::main );
	overlay.detach ( );

	int waited_for_window = 0;
	while ( !LI_FN ( FindWindowW )( _ ( L"UnityWndClass" ), nullptr ) )
	{
		if ( GetAsyncKeyState ( VK_END ) )
		{
			logging::print ( TYPE_WARNING, "VK_END pressed during window wait - exiting." );
			mist::other.release ( );
			mist::aim.release ( );
			mist::esp.release ( );
			mist::misc.release ( );
			mist::obj.release ( );
			mist::wep.release ( );
			pause_console ( "Exited by VK_END." );
			ExitProcess ( 0 );
		}
		if ( ( waited_for_window % 25 ) == 0 ) // every 5 seconds
			logging::print ( TYPE_LOG, "Still waiting for UnityWndClass (Rust window).." );
		++waited_for_window;
		sleep_for ( milliseconds ( 200 ) );
	}
	Beep ( 512, 256 );
	logging::print ( TYPE_DEBUG, "Found Rust window. Attaching to RustClient.exe.." );

	const bool attached = mist::other->attach ( h ( "RustClient.exe" ) );
	if ( !attached )
	{
		logging::print ( TYPE_ERROR, "attach(RustClient.exe) FAILED." );
		logging::print ( TYPE_ERROR, "GetLastError = %lu", GetLastError ( ) );
		logging::print ( TYPE_ERROR, "Common causes:" );
		logging::print ( TYPE_ERROR, "  - Not running as administrator" );
		logging::print ( TYPE_ERROR, "  - RustClient.exe isn't running yet" );
		logging::print ( TYPE_ERROR, "  - The h() compile-time hash doesn't match the process name (case/spelling)" );
		pause_console ( "Cannot attach to game. Aborting." );
		ExitProcess ( 1 );
	}
	logging::print ( TYPE_DEBUG, "Attach OK." );

	// Sanity check: read the first 2 bytes of the RustClient.exe image (should be 'MZ').
	// This proves ReadProcessMemory works against the game before we touch offsets.
	{
		const uintptr_t rust_base = mist::obj->find ( h ( "RustClient.exe" ) );
		if ( !rust_base )
		{
			logging::print ( TYPE_WARNING, "Could not resolve RustClient.exe module base for sanity check." );
		}
		else
		{
			uint16_t mz = mist::obj->read<uint16_t> ( rust_base );
			if ( mz == 0x5A4D ) // 'MZ'
				logging::print ( TYPE_DEBUG, "RPM sanity check OK: read 'MZ' at 0x%llX", (unsigned long long)rust_base );
			else
				logging::print ( TYPE_ERROR, "RPM sanity check FAILED: read 0x%04X (expected 0x5A4D 'MZ') at 0x%llX", mz, (unsigned long long)rust_base );
		}
	}

	logging::print ( TYPE_DEBUG, "Resolving module bases.." );
	object::setup ( );

	if ( !addr::unityplayer || !addr::gameassembly )
	{
		logging::print ( TYPE_ERROR, "Module base resolution FAILED:" );
		logging::print ( TYPE_ERROR, "  UnityPlayer.dll  = 0x%llX", (unsigned long long)addr::unityplayer );
		logging::print ( TYPE_ERROR, "  GameAssembly.dll = 0x%llX", (unsigned long long)addr::gameassembly );
		logging::print ( TYPE_ERROR, "If both are 0 the OpenProcess handle is dead (anti-cheat/permissions)." );
		logging::print ( TYPE_ERROR, "If one is 0 the game didn't load it yet - try joining a server first." );
		pause_console ( "Module resolution failed." );
		ExitProcess ( 2 );
	}
	logging::print ( TYPE_DEBUG, "UnityPlayer.dll  base = 0x%llX", (unsigned long long)addr::unityplayer );
	logging::print ( TYPE_DEBUG, "GameAssembly.dll base = 0x%llX", (unsigned long long)addr::gameassembly );
	logging::print ( TYPE_DEBUG, "base_networkable = 0x%llX", (unsigned long long)addr::base_networkable );
	logging::print ( TYPE_DEBUG, "client_entities  = 0x%llX", (unsigned long long)addr::client_entities );
	logging::print ( TYPE_DEBUG, "entity_realm     = 0x%llX", (unsigned long long)addr::entity_realm );
	logging::print ( TYPE_DEBUG, "buffer_list      = 0x%llX", (unsigned long long)addr::buffer_list );
	logging::print ( TYPE_DEBUG, "object_list      = 0x%llX (size=%zu)", (unsigned long long)addr::object_list, addr::object_size );

	if ( !addr::base_networkable )
		logging::print ( TYPE_WARNING, "base_networkable is 0 - bn_offset (0x%llX) is wrong for this game build, update offsets::bn_offset.",
			(unsigned long long)offsets::bn_offset );
	if ( !addr::client_entities )
		logging::print ( TYPE_WARNING, "client_entities is 0 - either offsets are wrong or you haven't joined a server yet." );

	// 2026-06-10 entity-walk diagnostic. Walks the chain step-by-
	// step using the ONLY decrypt + handle scheme this build advertises:
	//   sf=klass+0xB8 -> wrapper(sf+0x30) -> enc(wrapper+0x18) ->
	//   base_networkable (ADD/ROL29/SUB/XOR) -> resolve_tagged_handle (LSB scheme).
	if ( addr::base_networkable )
	{
		const uintptr_t klass   = addr::base_networkable;
		const uintptr_t sf      = mist::obj->read<uintptr_t> ( klass + offsets::cl_ent );
		const uintptr_t wrapper = sf ? mist::obj->read<uintptr_t> ( sf + offsets::element ) : 0;
		const uint64_t  enc     = wrapper ? mist::obj->read<uint64_t> ( wrapper + 0x18 ) : 0;

		// Apply the 2026-06-10 decrypt locally so the diagnostic is independent of sdk.cpp.
		uint64_t dec = enc;
		{
			uint32_t* c = reinterpret_cast<uint32_t*>( &dec );
			for ( int i = 0; i < 2; i++ )
			{
				uint32_t v = c[i];
				v += 0x9C7DA5DFu;
				v = ( v << 29 ) | ( v >> 3 );    // ROL 29
				v -= 0x1C287C47u;
				v ^= 0x595DCF40u;
				c[i] = v;
			}
		}

		const int      tag    = (int)( dec & 1 );
		const uint64_t result = ( !( dec & 1 ) && dec >= 0x10000ULL && dec <= 0x7FFFFFFFFFFFULL )
			? mist::obj->read<uint64_t> ( (uintptr_t)dec ) : 0;

		logging::print ( TYPE_DEBUG,
			"[v6] klass=0x%llX sf=0x%llX wrap=0x%llX enc=0x%016llX",
			(unsigned long long)klass, (unsigned long long)sf, (unsigned long long)wrapper,
			(unsigned long long)enc );
		logging::print ( TYPE_DEBUG,
			"[v6] decrypted=0x%016llX  LSB=%d (%s)  realm=0x%llX",
			(unsigned long long)dec, tag,
			tag ? "GCHandle-idx (NOT IMPL)" : "direct-slot",
			(unsigned long long)result );
	}

	// 2026-06-10 local-player static-slot diagnostic (works even if entity walk fails).
	{
		const uintptr_t lp_klass = mist::obj->read<uintptr_t> ( addr::gameassembly + offsets::local_player_slot_klass );
		const uintptr_t lp_sf    = lp_klass ? mist::obj->read<uintptr_t> ( lp_klass + offsets::cl_ent ) : 0;
		const uintptr_t lp       = lp_sf    ? mist::obj->read<uintptr_t> ( lp_sf + offsets::local_player_self_static_off ) : 0;
		logging::print ( TYPE_DEBUG,
			"[v6:local] klass=0x%llX sf=0x%llX  local_bp=0x%llX %s",
			(unsigned long long)lp_klass, (unsigned long long)lp_sf, (unsigned long long)lp,
			lp ? "<-- LOCAL PLAYER FOUND" : "(empty - not in server / not spawned)" );
	}

	std::thread keys ( misc::keybindh );
	keys.detach ( );

	std::thread objectloop ( object::main );
	objectloop.detach ( );

	logging::print ( TYPE_DEBUG, "Waiting for local player to enter game.." );
	int wait_count = 0;
	while ( game::m_rLocalInfo.m_pPlayer == 0 )
	{
		if ( ( wait_count % 25 ) == 0 ) // every ~5s
		{
			logging::print ( TYPE_LOG, "Still waiting for local player.. (object_list=0x%llX size=%zu)",
				(unsigned long long)addr::object_list, addr::object_size );
		}
		if ( GetAsyncKeyState ( VK_END ) )
		{
			logging::print ( TYPE_WARNING, "VK_END pressed during player wait - exiting." );
			pause_console ( "Exited by VK_END." );
			ExitProcess ( 0 );
		}
		++wait_count;
		sleep_for ( milliseconds ( 200 ) );
	}

	logging::print ( TYPE_DEBUG, "Local player entered: %p", (void*)game::m_rLocalInfo.m_pPlayer );
	std::thread aimbot ( aimbot::thread );
	aimbot.detach ( );

	std::thread misc ( misc::thread );
	misc.detach ( );

	Beep ( 500, 1250 );
	logging::print ( TYPE_DEBUG, "Have fun!" );

	while ( true )
	{
		if ( GetAsyncKeyState ( VK_END ) )
		{
			logging::print ( TYPE_WARNING, "VK_END pressed - exiting." );
			pause_console ( "Exited by VK_END." );
			ExitProcess ( 0 );
		}
		sleep_for ( 400ms );
	}
}

#ifdef _WINDLL
BOOL WINAPI DllMain ( HMODULE hDll, DWORD dwReason, PVOID )
{
	if ( dwReason == DLL_PROCESS_ATTACH )
		CreateThread ( 0, NULL, MainThread, 0, NULL, NULL );
	return true;
}
#endif

#define _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
#include "win_utils.hpp"
#include "etc_utils.hpp"

#include <chrono>
#include <thread>

#pragma comment(lib, "LDE64x64.lib")

EXTERN_C int LDE( void*, int );

int main( int argc, char** argv )
{
	if ( argc != 3 )
	{
		printf( "[!] incorrect usage\n[!] format: splendid_implanter.exe [dll_name] [window_class]\n" );
		return -1;
	}

	if ( !std::filesystem::exists( argv[ 1 ] ) )
	{
		printf( "[!] dll path supplied is invalid\n" );
		return -1;
	}

	printf( "[~] welcome to spenldid implanter poc\n" );

	// we need to enable the debug privilege
	if ( !impl::enable_privilege( L"SeDebugPrivilege" ) )
		return -1;

	printf( "[~] enabled debug privilege!\n" );

	namespace th = std::this_thread; 
	namespace ch = std::chrono;

	auto be_process_id = 0;

	while ( !be_process_id )
	{
		CALL_ONCE( [ ]( )
				   {
					   printf( "[~] waiting for battleye service...\n" );
				   } );

		// check if it's been 2 minutes since start, if it's been 2 minutes & process hasn't been found, break
		static const auto cached_time = ch::system_clock::now( );
		const auto current_time_mins = ch::duration_cast< ch::minutes >( ch::system_clock::now( ) - cached_time ).count( );

		if ( current_time_mins >= 2u )
			break;

		be_process_id = impl::get_process_id( L"BEService.exe" );
		th::sleep_for( ch::milliseconds( 20 ) );
	}

	if ( !be_process_id )
	{
		printf( "[!] timed out\n" );
		return -1;
	}

	printf( "[~] found BEService process [%lu]\n", be_process_id );

	// open handle to beservice
	impl::uq_handle be_process_handle{ OpenProcess( PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, be_process_id ), &CloseHandle };

	// return value is NULL incase of failure
	if ( !be_process_handle.get( ) )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	printf( "[~] opened process handle [0x%p]\n", be_process_handle.get( ) );

	// retrieve module data
	const auto be_data = impl::get_module_data( be_process_handle.get( ), L"BEService.exe" );

	// shouldn't be possible
	if ( !be_data.first )
	{
		printf( "[!] failed to find BEService.exe data\n" );
		return -1;
	}

	printf( "[~] retrieved BEService address [0x%p]\n", be_data.first );

	// get handle to file on disk
	impl::uq_handle be_disk_handle{ CreateFileW( be_data.second.c_str( ), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, INVALID_HANDLE_VALUE ), &CloseHandle };

	// return value is INVALID_HANDLE_VALUE incase of failure
	if ( be_disk_handle.get( ) == INVALID_HANDLE_VALUE )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	printf( "[~] opened disk handle [0x%p]\n", be_disk_handle.get( ) );

	auto be_disk_buffer = impl::get_file_data( be_disk_handle.get( ), be_data.second );

	// internally logged
	//if ( be_disk_buffer.empty( ) )
	//	return -1;

	// look for an executable section to deploy hook in
	const auto nt_header = reinterpret_cast< PIMAGE_NT_HEADERS >( be_disk_buffer.data( ) + reinterpret_cast< PIMAGE_DOS_HEADER >( be_disk_buffer.data( ) )->e_lfanew );
	const auto section_header = reinterpret_cast< PIMAGE_SECTION_HEADER >( nt_header + 1 );

	PIMAGE_SECTION_HEADER executable_section = nullptr;

	for ( auto i = 0; i < nt_header->FileHeader.NumberOfSections; i++ )
	{
		auto& curr_section = section_header[ i ];

		if ( curr_section.SizeOfRawData != 0 )
			continue;

		if ( !( curr_section.Characteristics & IMAGE_SCN_MEM_EXECUTE ) )
			continue;

		executable_section = &curr_section;
		break;
	}

	if ( !executable_section )
	{
		printf( "[!] can't find needed section...\n" );
		return -1;
	}

	printf( "[~] found section [%s]\n", reinterpret_cast< const char* >( executable_section->Name ) );

	const auto short_dll_path = impl::to_utf16( argv[ 1 ] );

	// since w10 1607, the limit for maximum path isn't actually MAX_PATH
	auto dll_path = std::make_unique<wchar_t[ ]>( MAX_PATH );
	GetFullPathNameW( short_dll_path.c_str( ), MAX_PATH, dll_path.get( ), nullptr );

	printf( "[~] dll path: %ws\n", dll_path.get( ) );

	const auto dll_path_sz = wcslen( dll_path.get( ) ) * 2;

	auto kernel_path = std::make_unique<wchar_t[ ]>( MAX_PATH );
	GetModuleFileNameW( GetModuleHandleW( L"Kernel32.dll" ), kernel_path.get( ), MAX_PATH );

	printf( "[~] kernel32 path: %ws\n", kernel_path.get( ) );

	const auto kernel_path_sz = wcslen( kernel_path.get( ) ) * 2;

	// allocate a buffer in the process to hold our paths
	const auto paths_buffer = VirtualAllocEx( be_process_handle.get( ), nullptr, dll_path_sz + kernel_path_sz + 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );

	if ( !paths_buffer )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	printf( "[~] allocated buffer at 0x%p\n", paths_buffer );

	RET_CHK( WriteProcessMemory( be_process_handle.get( ), paths_buffer, dll_path.get( ), dll_path_sz, nullptr ) )

	printf( "[~] wrote dll path successfully!\n" );

	const auto second_path_buffer = reinterpret_cast< uint8_t* >( paths_buffer ) + dll_path_sz + 2;

	RET_CHK( WriteProcessMemory( be_process_handle.get( ), second_path_buffer, kernel_path.get( ), kernel_path_sz, nullptr ) )

	printf( "[~] wrote kernel32 path successfully!\n" );

	uint8_t jmp_stub[ ]
	{
		0x51, 0x48, 0xb9, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
		0xcc, 0xcc, 0x48, 0x87, 0x0c, 0x24, 0xc3
	};

	// :), figure this out
	uint8_t shell_code[ ]
	{
		0x41, 0x54, 0x41, 0x55, 0x49, 0x89, 0xca, 0x49, 0xbc,
		0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x4d,
		0x0f, 0xb7, 0x2c, 0x24, 0x4d, 0x85, 0xed, 0x74, 0x1e,
		0x4d, 0x0f, 0xb7, 0x1a, 0x4d, 0x85, 0xdb, 0x74, 0x1f,
		0x4d, 0x39, 0xeb, 0x75, 0x0a, 0x49, 0x83, 0xc2, 0x02,
		0x49, 0x83, 0xc4, 0x02, 0xeb, 0xde, 0x49, 0x83, 0xc2,
		0x02, 0xeb, 0xce, 0x48, 0xb9, 0xcc, 0xcc, 0xcc, 0xcc,
		0xcc, 0xcc, 0xcc, 0xcc, 0x41, 0x5d, 0x41, 0x5c
	};

	const auto export_address = reinterpret_cast< uint8_t* >( GetProcAddress( GetModuleHandleW( L"Kernelbase.dll" ), "CreateFileW" ) );

	printf( "[~] preparing hook...\n" );

	// calculate buffer length based on bytes needed from original
	auto og_len = 0;

	while ( og_len < sizeof( jmp_stub ) )
		og_len += LDE( export_address + og_len, 64 );

	const auto buf_len = og_len + sizeof( shell_code ) + sizeof( jmp_stub );
	const auto buf_data = std::make_unique<uint8_t[ ]>( buf_len ).get( );
	const auto og_data = export_address + og_len;

	// :)
	memcpy( buf_data, shell_code, sizeof( shell_code ) );
	memcpy( buf_data + 0x9, &paths_buffer, 8 );
	memcpy( buf_data + 0x3b, &second_path_buffer, 8 );
	memcpy( buf_data + sizeof( shell_code ), export_address, og_len );
	memcpy( buf_data + sizeof( shell_code ) + og_len, jmp_stub, sizeof( jmp_stub ) );
	memcpy( buf_data + sizeof( shell_code ) + og_len + 3, &og_data, 8 );

	const auto deployment_location = ( reinterpret_cast< uint8_t* >( be_data.first ) + executable_section->VirtualAddress + executable_section->Misc.VirtualSize ) - buf_len;

	printf( "[~] deploying hook...\n" );

	auto cache = 0;

	RET_CHK( VirtualProtectEx( be_process_handle.get( ), deployment_location, buf_len, PAGE_EXECUTE_READWRITE, reinterpret_cast< PDWORD >( &cache ) ) )
	RET_CHK( WriteProcessMemory( be_process_handle.get( ), deployment_location, buf_data, buf_len, nullptr ) )
	RET_CHK( VirtualProtectEx( be_process_handle.get( ), deployment_location, buf_len, cache, reinterpret_cast< PDWORD >( &cache ) ) )

	*reinterpret_cast< uint64_t* >( &jmp_stub[ 3 ] ) = reinterpret_cast< uint64_t >( deployment_location );

	RET_CHK( WriteProcessMemory( be_process_handle.get( ), export_address, jmp_stub, sizeof( jmp_stub ), nullptr ) )

	printf( "[~] hook deployed!\n" );

	HWND game_window = nullptr;

	while ( !game_window )
	{
		CALL_ONCE( [ ]( )
				   {
					   printf( "[~] waiting for game to open...\n" );
				   } );

		// check if it's been 2 minutes since start, if it's been 2 minutes & window hasn't been found, break
		static const auto cached_time = ch::system_clock::now( );
		const auto current_time_mins = ch::duration_cast< ch::minutes >( ch::system_clock::now( ) - cached_time ).count( );

		if ( current_time_mins >= 2u )
			break;

		game_window = FindWindowA( argv[ 2 ], nullptr );
		th::sleep_for( ch::milliseconds( 250 ) );
	}

	if ( !game_window )
	{
		printf( "[!] timed out\n" );
		return -1;
	}

	const auto window_thread = GetWindowThreadProcessId( game_window, nullptr );

	if ( !window_thread )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	const auto loaded_module = LoadLibraryW( short_dll_path.c_str( ) );

	if ( !loaded_module )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	const auto window_hook = GetProcAddress( loaded_module, "wnd_hk" );

	if ( !window_hook )
	{
		printf( "[!] can't find needed export in implanted dll\n" );
		return -1;
	}

	// handle failure etc...
	for ( auto i = 0; i < 50; i++ )
		PostThreadMessageW( window_thread, 0x5b0, 0, reinterpret_cast< LPARAM >( shell_code ) );

	printf( "[~] dll implanted\n" );

	printf( "[~] splendid implanter out!\n" );

	return 0;
}
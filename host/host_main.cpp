#include "host_session.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/remote_protocol.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/upscalers/native_dlss.hpp"

#include <Windows.h>
#include <delayimp.h>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>

#ifndef AEONSR_VERSION_MAJOR
#define AEONSR_VERSION_MAJOR 1
#endif
#ifndef AEONSR_VERSION_MINOR
#define AEONSR_VERSION_MINOR 1
#endif

namespace {

FARPROC WINAPI system32_only(unsigned notify, PDelayLoadInfo info)
{
	if (notify != dliNotePreLoadLibrary || info == nullptr || info->szDll == nullptr)
		return nullptr;
	wchar_t name[MAX_PATH] = {};
	if (MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, name, MAX_PATH) == 0)
		return nullptr;
	return reinterpret_cast<FARPROC>(LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
}

}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = system32_only;

namespace {

using namespace aeon_sr;
using aeon_sr::host::HostSession;
using aeon_sr::host::copy_text;

const wchar_t *const kSystemFirst[] = {
	L"dxgi.dll", L"d3d12.dll", L"d3d11.dll", L"D3DCOMPILER_47.dll", L"bcrypt.dll",
	L"version.dll", L"winmm.dll", L"dbghelp.dll", L"wininet.dll", L"winhttp.dll",
};

void load_system_first()
{
	for (const wchar_t *name : kSystemFirst)
		LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

void note_foreign_modules(const wchar_t *when)
{
	std::wstring found;
	for (const NgxLayer &l : scan_ngx_layers(nullptr))
		found += (found.empty() ? L"" : L", ") + ngx_layer_label(l) + L" at " + l.path;
	HMODULE mods[512];
	DWORD need = 0;
	if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
		const DWORD n = std::min<DWORD>(need / sizeof(HMODULE), 512);
		for (DWORD i = 0; i < n; ++i) {
			if (GetProcAddress(mods[i], "ReShadeRegisterAddon") == nullptr)
				continue;
			wchar_t path[MAX_PATH * 2] = {};
			GetModuleFileNameW(mods[i], path, MAX_PATH * 2);
			found += (found.empty() ? L"ReShade at " : L", ReShade at ") + std::wstring(path);
		}
	}
	if (found.empty())
		diag_logf(DiagLevel::Info, "host", L"%ls: no program of the game's in this process", when);
	else
		diag_logf(DiagLevel::Warn, "host", L"%ls: loaded in this process from the game's folder: %ls", when,
			found.c_str());
}

enum Exit : int {
	ExitOk = 0,
	ExitNoPid = 2,
	ExitNoGame = 3,
	ExitNoSession = 4,
	ExitHandshake = 5,
	ExitEngine = 6,
	ExitWaitFailed = 7,
};

struct Args {
	uint32_t pid = 0;
	std::wstring log;
	bool verbose = false;
};

Args parse_args()
{
	Args args;
	int argc = 0;
	LPWSTR *const argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv == nullptr)
		return args;
	for (int i = 1; i < argc; ++i) {
		if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc)
			args.pid = static_cast<uint32_t>(wcstoul(argv[++i], nullptr, 10));
		else if (wcscmp(argv[i], L"--log") == 0 && i + 1 < argc)
			args.log = argv[++i];
		else if (wcscmp(argv[i], L"--verbose") == 0)
			args.verbose = true;
	}
	LocalFree(argv);
	return args;
}

struct Kernel {
	HANDLE game = nullptr;
	HANDLE mapping = nullptr;
	HANDLE request = nullptr;
	HANDLE response = nullptr;
	remote::Block *block = nullptr;

	~Kernel()
	{
		if (block != nullptr)
			UnmapViewOfFile(block);
		if (response != nullptr)
			CloseHandle(response);
		if (request != nullptr)
			CloseHandle(request);
		if (mapping != nullptr)
			CloseHandle(mapping);
		if (game != nullptr)
			CloseHandle(game);
	}
};

bool game_gone(HANDLE game) noexcept
{
	return WaitForSingleObject(game, 0) == WAIT_OBJECT_0;
}

HANDLE open_with_retry(HANDLE game, const wchar_t *name, bool mapping)
{
	const ULONGLONG deadline = GetTickCount64() + remote::kHandshakeTimeoutMs;
	for (;;) {
		HANDLE h = mapping
			? OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name)
			: OpenEventW(EVENT_ALL_ACCESS, FALSE, name);
		if (h != nullptr)
			return h;
		if (game_gone(game))
			return nullptr;
		if (GetTickCount64() >= deadline)
			return nullptr;
		Sleep(10);
	}
}

std::wstring number(unsigned long long value)
{
	wchar_t buf[32]{};
	swprintf(buf, 32, L"%llu", value);
	return buf;
}

int run(const Args &args, const std::wstring &host_dir)
{
	diag_info("host", L"AeonSRHost starting for game " + number(args.pid));

	Kernel k;
	k.game = OpenProcess(SYNCHRONIZE, FALSE, args.pid);
	if (k.game == nullptr) {
		diag_error("host", L"the game process " + number(args.pid) +
			L" could not be opened, so there is nothing to serve.");
		return ExitNoGame;
	}

	remote::SessionNames names;
	remote::session_names(args.pid, names);

	k.mapping = open_with_retry(k.game, names.block, true);
	if (k.mapping == nullptr) {
		diag_error("host", L"the game never published a session block under " +
			std::wstring(names.block) + L".");
		return ExitNoSession;
	}

	k.block = static_cast<remote::Block *>(
		MapViewOfFile(k.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(remote::Block)));
	if (k.block == nullptr) {
		diag_error("host", L"the game's session block is not the size this engine host was "
			L"built for. The add-on and the host are from different builds.");
		return ExitHandshake;
	}
	remote::Block *const block = k.block;

	const uint32_t magic = block->magic;
	const uint32_t version = block->version;
	const uint32_t block_size = block->block_size;
	if (magic != remote::kMagic || version != remote::kProtocolVersion ||
		block_size != remote::kBlockSize) {
		const std::wstring why = L"the add-on and the engine host are from different builds. "
			L"Reinstall both from the same package.";
		block->step = static_cast<uint32_t>(remote::RemoteStep::Handshake);
		copy_text(block->message, remote::kTextChars, why);
		diag_error("host", why + L" (magic " + number(magic) + L", version " + number(version) +
			L", block " + number(block_size) + L"; this host wants " + number(remote::kMagic) +
			L", " + number(remote::kProtocolVersion) + L", " + number(remote::kBlockSize) + L")");
		return ExitHandshake;
	}

	k.request = open_with_retry(k.game, names.request, false);
	k.response = open_with_retry(k.game, names.response, false);
	if (k.request == nullptr || k.response == nullptr) {
		diag_error("host", L"the game never published the two events a frame is handed over "
			L"with, so no frame can be served.");
		return ExitNoSession;
	}

	note_foreign_modules(L"before the engine");
	HostSession session;
	const bool started = session.start(args.pid, block, host_dir);
	note_foreign_modules(L"with the engine up");
	if (!started) {
		block->host_ready = 0;
		block->step = static_cast<uint32_t>(remote::RemoteStep::Engine);
		copy_text(block->message, remote::kTextChars, session.last_error());
		diag_error("host", session.last_error());
		SetEvent(k.response);
		return ExitEngine;
	}

	{
		const aeon_sr::host::HostCapabilities caps = session.capabilities();
		block->backends_present = caps.backends_present;
		block->neural_present = caps.neural_present;
		block->host_gpu_vendor = caps.gpu_vendor;
		block->reserved_handshake = 0;
	}
	block->host_pid = GetCurrentProcessId();
	block->host_version_major = AEONSR_VERSION_MAJOR;
	block->host_version_minor = AEONSR_VERSION_MINOR;
	MemoryBarrier();
	block->host_ready = 1;
	diag_info("host", L"engine host " + number(GetCurrentProcessId()) + L" ready for game " +
		number(args.pid));

	int code = ExitOk;
	HANDLE waits[2] = { k.request, k.game };
	for (;;) {
		const DWORD hit = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
		if (hit == WAIT_OBJECT_0) {
			if (session.shutdown_requested()) {
				diag_info("host", L"the game asked the engine host to close.");
				block->host_ready = 0;
				MemoryBarrier();
				SetEvent(k.response);
				break;
			}
			session.serve_frame();
			SetEvent(k.response);
			continue;
		}
		if (hit == WAIT_OBJECT_0 + 1) {
			diag_info("host", L"the game exited; the engine host is shutting down.");
			break;
		}
		diag_error("host", L"the engine host lost the handles it serves frames on.");
		code = ExitWaitFailed;
		break;
	}

	block->host_ready = 0;
	session.shutdown();
	return code;
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
	load_system_first();
	const Args args = parse_args();

	const std::wstring host_dir = module_directory(GetModuleHandleW(nullptr));
	diag_open_log(args.log.empty() ? join_path(host_dir, L"AeonSRHost.log") : args.log);
	diag_set_verbose(args.verbose);

	int code = ExitNoPid;
	if (args.pid == 0)
		diag_error("host", L"AeonSRHost needs --pid <game process id> and was given none.");
	else
		code = run(args, host_dir);

	diag_close_log();
	return code;
}

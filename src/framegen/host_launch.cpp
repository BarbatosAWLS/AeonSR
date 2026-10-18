#include "aeon_sr/framegen/host_launch.hpp"

#include "aeon_sr/core/settings.hpp"

#include <cwchar>
#include <vector>

namespace aeon_sr {

namespace {

std::wstring error_text(const wchar_t *what, DWORD error)
{
	wchar_t buf[160];
	swprintf(buf, 160, L"%ls (error %lu)", what, static_cast<unsigned long>(error));
	return buf;
}

HMODULE this_module() noexcept
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&this_module), &self);
	return self;
}

}

bool launch_host_suspended(const std::wstring &exe, HANDLE mapping, const wchar_t *mode, const wchar_t *log_name,
	HostLaunch *out, std::wstring *why)
{
	*out = HostLaunch{};
	if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
		*why = L"AeonSRHost.exe is not beside the add-on";
		return false;
	}
	SIZE_T list_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
	std::vector<unsigned char> list_mem(list_size);
	auto *const list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_mem.data());
	HANDLE inherit[1] = { mapping };
	if (!InitializeProcThreadAttributeList(list, 1, 0, &list_size)) {
		*why = error_text(L"AeonSRHost.exe could not be given the block", GetLastError());
		return false;
	}
	if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
		*why = error_text(L"AeonSRHost.exe could not be given the block", GetLastError());
		DeleteProcThreadAttributeList(list);
		return false;
	}
	wchar_t args[96];
	swprintf(args, 96, L"%ls %llu", mode, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mapping)));
	std::wstring dir = exe;
	const size_t slash = dir.find_last_of(L"\\/");
	dir = slash != std::wstring::npos ? dir.substr(0, slash) : std::wstring(L".");
	std::wstring command = L"\"" + exe + L"\"" + args + L" --log \"" + dir + L"\\" + log_name + L"\"";
	std::vector<wchar_t> mutable_command(command.begin(), command.end());
	mutable_command.push_back(L'\0');
	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof(si);
	si.lpAttributeList = list;
	PROCESS_INFORMATION pi{};
	DWORD priority = GetPriorityClass(GetCurrentProcess());
	if (priority == REALTIME_PRIORITY_CLASS || priority == 0)
		priority = HIGH_PRIORITY_CLASS;
	SetHandleInformation(mapping, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	const BOOL created = CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
		CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | priority, nullptr, dir.c_str(),
		&si.StartupInfo, &pi);
	const DWORD create_error = GetLastError();
	SetHandleInformation(mapping, HANDLE_FLAG_INHERIT, 0);
	DeleteProcThreadAttributeList(list);
	if (!created) {
		*why = error_text(L"AeonSRHost.exe would not start", create_error);
		return false;
	}
	out->process = pi.hProcess;
	out->thread = pi.hThread;
	out->pid = pi.dwProcessId;
	return true;
}

uint64_t give_handle(HANDLE process, HANDLE h, DWORD access, bool same, bool *ok) noexcept
{
	HANDLE out = nullptr;
	if (h == nullptr || !DuplicateHandle(GetCurrentProcess(), h, process, &out, access, FALSE,
			same ? DUPLICATE_SAME_ACCESS : 0)) {
		*ok = false;
		return 0;
	}
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(out));
}

std::wstring host_beside_addon(const wchar_t *env)
{
	wchar_t v[MAX_PATH] = {};
	const DWORD n = env != nullptr ? GetEnvironmentVariableW(env, v, MAX_PATH) : 0;
	if (n > 0 && n < MAX_PATH)
		return wcscmp(v, L"0") == 0 ? std::wstring() : std::wstring(v);
	return join_path(module_directory(this_module()), L"AeonSRHost.exe");
}

}

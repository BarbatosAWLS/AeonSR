#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace aeon_sr {

struct HostLaunch {
	HANDLE process = nullptr;
	HANDLE thread = nullptr;
	uint32_t pid = 0;
};

bool launch_host_suspended(const std::wstring &exe, HANDLE mapping, const wchar_t *mode, const wchar_t *log_name,
	HostLaunch *out, std::wstring *why);

uint64_t give_handle(HANDLE process, HANDLE h, DWORD access, bool same, bool *ok) noexcept;

std::wstring host_beside_addon(const wchar_t *env);

}

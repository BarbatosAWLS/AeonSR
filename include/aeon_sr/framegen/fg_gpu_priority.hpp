#pragma once

#include <Windows.h>

#include <cctype>
#include <cstring>
#include <string>

namespace aeon_sr {

enum class GpuClass : int {
	Idle = 0,
	BelowNormal = 1,
	Normal = 2,
	AboveNormal = 3,
	High = 4,
	Realtime = 5,
};

inline const wchar_t *gpu_class_name(GpuClass c) noexcept
{
	switch (c) {
	case GpuClass::Idle: return L"idle";
	case GpuClass::BelowNormal: return L"below normal";
	case GpuClass::Normal: return L"normal";
	case GpuClass::AboveNormal: return L"above normal";
	case GpuClass::High: return L"high";
	case GpuClass::Realtime: return L"realtime";
	}
	return L"unknown";
}

inline GpuClass window_gpu_class_wanted() noexcept
{
	char v[32] = {};
	const DWORD n = GetEnvironmentVariableA("AEONSR_FG_GPU_PRIORITY", v, sizeof(v));
	if (n == 0 || n >= sizeof(v))
		return GpuClass::Realtime;
	std::string s;
	for (const char *p = v; *p != '\0'; ++p)
		if (!std::isspace(static_cast<unsigned char>(*p)))
			s += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
	if (s == "normal" || s == "0")
		return GpuClass::Normal;
	if (s == "high")
		return GpuClass::High;
	return GpuClass::Realtime;
}

namespace fg_gpu_detail {

template <typename Fn>
Fn kmt(const char *name) noexcept
{
	HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
	if (gdi == nullptr)
		gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	return gdi != nullptr ? reinterpret_cast<Fn>(GetProcAddress(gdi, name)) : nullptr;
}

}

inline bool get_gpu_class(HANDLE process, GpuClass *out) noexcept
{
	using GetClass = LONG(APIENTRY *)(HANDLE, int *);
	static const GetClass get = fg_gpu_detail::kmt<GetClass>("D3DKMTGetProcessSchedulingPriorityClass");
	int c = -1;
	if (get == nullptr || get(process, &c) != 0 || c < static_cast<int>(GpuClass::Idle) ||
		c > static_cast<int>(GpuClass::Realtime))
		return false;
	*out = static_cast<GpuClass>(c);
	return true;
}

inline LONG set_own_gpu_class(GpuClass wanted, GpuClass *now, bool *known) noexcept
{
	using SetClass = LONG(APIENTRY *)(HANDLE, int);
	static const SetClass set = fg_gpu_detail::kmt<SetClass>("D3DKMTSetProcessSchedulingPriorityClass");
	const LONG status = set != nullptr ? set(GetCurrentProcess(), static_cast<int>(wanted)) : -1;
	*known = get_gpu_class(GetCurrentProcess(), now);
	return status;
}

}

#include "aeon_sr/upscalers/native_dlss.hpp"

#include <Windows.h>

namespace aeon_sr {

namespace {

NativeDlss g_at_load;

std::wstring module_path(HMODULE m)
{
	wchar_t buf[MAX_PATH]{};
	const DWORD n = GetModuleFileNameW(m, buf, MAX_PATH);
	return n > 0 && n < MAX_PATH ? std::wstring(buf) : std::wstring();
}

}

NativeDlss scan_native_dlss()
{
	struct Entry {
		const wchar_t *name;
		bool NativeDlss::*flag;
	};
	static const Entry kEntries[] = {
		{ L"sl.interposer.dll", &NativeDlss::streamline },
		{ L"sl.dlss.dll", &NativeDlss::streamline },
		{ L"sl.dlss_g.dll", &NativeDlss::streamline },
		{ L"nvngx_dlss.dll", &NativeDlss::dlss },
		{ L"nvngx_dlssg.dll", &NativeDlss::frame_generation },
		{ L"nvngx_dlssd.dll", &NativeDlss::ray_reconstruction },
		{ L"_nvngx.dll", &NativeDlss::ngx_core },
	};

	NativeDlss out;
	out.queried = true;
	for (const Entry &e : kEntries) {
		const HMODULE m = GetModuleHandleW(e.name);
		if (m == nullptr)
			continue;
		out.*(e.flag) = true;
		if (!out.modules.empty())
			out.modules += L", ";
		out.modules += e.name;
		if (e.flag == &NativeDlss::dlss)
			out.dlss_path = module_path(m);
	}
	return out;
}

void snapshot_native_dlss()
{
	g_at_load = scan_native_dlss();
}

const NativeDlss &native_dlss_at_load() noexcept
{
	return g_at_load;
}

}

#pragma once

#include <Windows.h>

namespace aeon_sr {

inline void quiet_tool() noexcept
{
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
}

inline HWND quiet_window(const wchar_t *cls, HINSTANCE inst) noexcept
{
	return CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls, cls,
		WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, inst, nullptr);
}

}

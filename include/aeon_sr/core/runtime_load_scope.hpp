#pragma once

#include <windows.h>

#include <atomic>
#include <cwchar>

namespace aeon_sr {

inline std::atomic<int> g_runtime_loads{ 0 };

inline bool runtime_load_active() noexcept
{
	return g_runtime_loads.load(std::memory_order_acquire) != 0;
}

inline DWORD runtime_load_test_hold_ms() noexcept
{
	wchar_t v[16]{};
	const DWORD n = GetEnvironmentVariableW(L"AEONSR_TEST_LOADER_HOLD_MS", v, 16);
	return n == 0 || n >= 16 ? 0u : static_cast<DWORD>(std::wcstoul(v, nullptr, 10));
}

class RuntimeLoadScope {
public:
	RuntimeLoadScope() noexcept
	{
		g_runtime_loads.fetch_add(1, std::memory_order_acq_rel);
		if (const DWORD hold = runtime_load_test_hold_ms())
			hold_loader_lock(hold);
	}
	~RuntimeLoadScope() { g_runtime_loads.fetch_sub(1, std::memory_order_acq_rel); }
	RuntimeLoadScope(const RuntimeLoadScope &) = delete;
	RuntimeLoadScope &operator=(const RuntimeLoadScope &) = delete;

private:
	static void hold_loader_lock(DWORD ms) noexcept
	{
		using Lock = LONG(NTAPI *)(ULONG, ULONG *, ULONG_PTR *);
		using Unlock = LONG(NTAPI *)(ULONG, ULONG_PTR);
		const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		const auto lock = ntdll != nullptr
			? reinterpret_cast<Lock>(reinterpret_cast<void *>(GetProcAddress(ntdll, "LdrLockLoaderLock"))) : nullptr;
		const auto unlock = ntdll != nullptr
			? reinterpret_cast<Unlock>(reinterpret_cast<void *>(GetProcAddress(ntdll, "LdrUnlockLoaderLock"))) : nullptr;
		ULONG_PTR cookie = 0;
		if (lock == nullptr || unlock == nullptr || lock(0, nullptr, &cookie) < 0)
			return;
		Sleep(ms);
		unlock(0, cookie);
	}
};

}

#include "aeon_sr/framegen/present_sync.hpp"

#include <Windows.h>
#include <dxgi1_2.h>

#include <atomic>
#include <mutex>

namespace aeon_sr::present_sync {

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);

constexpr size_t kPresentIndex = 8;
constexpr size_t kPresent1Index = 22;
constexpr uint32_t kTables = 4;

struct Patch {
	std::atomic<void **> slot{ nullptr };
	std::atomic<void *> original{ nullptr };
	bool live = false;
};

Patch g_present[kTables];
Patch g_present1[kTables];
std::mutex g_mutex;
std::atomic<void *> g_watched{ nullptr };
std::atomic<bool> g_force{ false };
std::atomic<uint32_t> g_game_sync{ kUnknown };
std::atomic<uint64_t> g_presents{ 0 };
std::atomic<uint32_t> g_sent_sync{ kUnknown };
thread_local int t_own = 0;

void **slot_of(void *object, size_t index) noexcept
{
	return *reinterpret_cast<void ***>(object) + index;
}

void *original_of(Patch *table, void *self, size_t index) noexcept
{
	void **const slot = slot_of(self, index);
	void *first = nullptr;
	for (uint32_t i = 0; i < kTables; ++i) {
		void *const original = table[i].original.load(std::memory_order_acquire);
		if (table[i].slot.load(std::memory_order_acquire) == slot)
			return original;
		if (first == nullptr)
			first = original;
	}
	return first;
}

void pin_self() noexcept
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		reinterpret_cast<LPCWSTR>(&slot_of), &self);
}

bool is_game_present(void *self, UINT flags) noexcept
{
	return t_own == 0 && self == g_watched.load(std::memory_order_acquire) && (flags & DXGI_PRESENT_TEST) == 0;
}

void note_game_present(UINT *sync, UINT *flags) noexcept
{
	g_game_sync.store(*sync, std::memory_order_relaxed);
	g_presents.fetch_add(1, std::memory_order_relaxed);
	uint32_t s = *sync, f = *flags;
	apply(g_force.load(std::memory_order_acquire), &s, &f);
	*sync = s;
	*flags = f;
	g_sent_sync.store(s, std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE present_detour(IDXGISwapChain *self, UINT sync, UINT flags)
{
	const auto original = reinterpret_cast<PresentFn>(original_of(g_present, self, kPresentIndex));
	if (original == nullptr)
		return DXGI_ERROR_INVALID_CALL;
	if (is_game_present(self, flags))
		note_game_present(&sync, &flags);
	return original(self, sync, flags);
}

HRESULT STDMETHODCALLTYPE present1_detour(IDXGISwapChain1 *self, UINT sync, UINT flags,
	const DXGI_PRESENT_PARAMETERS *params)
{
	const auto original = reinterpret_cast<Present1Fn>(original_of(g_present1, self, kPresent1Index));
	if (original == nullptr)
		return DXGI_ERROR_INVALID_CALL;
	if (is_game_present(self, flags))
		note_game_present(&sync, &flags);
	return original(self, sync, flags, params);
}

bool write_slot(void **slot, void *value) noexcept
{
	MEMORY_BASIC_INFORMATION mbi{};
	if (VirtualQuery(slot, &mbi, sizeof(mbi)) == 0)
		return false;
	const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	DWORD old = 0;
	if (!VirtualProtect(slot, sizeof(void *), (mbi.Protect & exec) != 0 ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old))
		return false;
	InterlockedExchangePointer(slot, value);
	DWORD unused = 0;
	VirtualProtect(slot, sizeof(void *), old, &unused);
	return true;
}

bool patch(Patch *table, void *object, size_t index, void *detour) noexcept
{
	void **const slot = slot_of(object, index);
	uint32_t empty = kTables;
	for (uint32_t i = 0; i < kTables; ++i) {
		void **const s = table[i].slot.load(std::memory_order_acquire);
		if (s == slot) {
			if (table[i].live)
				return true;
			if (*slot != table[i].original.load(std::memory_order_acquire) || !write_slot(slot, detour))
				return false;
			table[i].live = true;
			return true;
		}
		if (s == nullptr && empty == kTables)
			empty = i;
	}
	if (empty == kTables || *slot == detour)
		return false;
	table[empty].original.store(*slot, std::memory_order_release);
	table[empty].slot.store(slot, std::memory_order_release);
	if (!write_slot(slot, detour)) {
		table[empty].slot.store(nullptr, std::memory_order_release);
		table[empty].original.store(nullptr, std::memory_order_release);
		return false;
	}
	table[empty].live = true;
	pin_self();
	return true;
}

void restore(Patch *table, void *detour) noexcept
{
	for (uint32_t i = 0; i < kTables; ++i) {
		void **const slot = table[i].slot.load(std::memory_order_acquire);
		if (slot == nullptr || !table[i].live)
			continue;
		if (*slot == detour && write_slot(slot, table[i].original.load(std::memory_order_acquire)))
			table[i].live = false;
	}
}

}

void apply(bool forcing, uint32_t *sync_interval, uint32_t *flags) noexcept
{
	if (!forcing || (*flags & DXGI_PRESENT_DO_NOT_WAIT) != 0)
		return;
	*sync_interval = 1;
	*flags &= ~static_cast<uint32_t>(DXGI_PRESENT_ALLOW_TEARING);
}

bool watch(void *native_swapchain) noexcept
{
	if (native_swapchain == nullptr)
		return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	auto *const sc = static_cast<IDXGISwapChain *>(native_swapchain);
	if (!patch(g_present, sc, kPresentIndex, reinterpret_cast<void *>(&present_detour)))
		return false;
	IDXGISwapChain1 *sc1 = nullptr;
	if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1 != nullptr) {
		const bool ok = patch(g_present1, sc1, kPresent1Index, reinterpret_cast<void *>(&present1_detour));
		sc1->Release();
		if (!ok)
			return false;
	}
	if (g_watched.exchange(native_swapchain, std::memory_order_acq_rel) != native_swapchain) {
		g_game_sync.store(kUnknown, std::memory_order_relaxed);
		g_sent_sync.store(kUnknown, std::memory_order_relaxed);
		g_presents.store(0, std::memory_order_relaxed);
	}
	return true;
}

void forget() noexcept
{
	g_force.store(false, std::memory_order_release);
	g_watched.store(nullptr, std::memory_order_release);
	g_game_sync.store(kUnknown, std::memory_order_relaxed);
	g_sent_sync.store(kUnknown, std::memory_order_relaxed);
	g_presents.store(0, std::memory_order_relaxed);
}

void force_vsync(bool on) noexcept
{
	g_force.store(on, std::memory_order_release);
}

uint32_t game_sync_interval() noexcept
{
	return g_game_sync.load(std::memory_order_relaxed);
}

uint32_t sent_sync_interval() noexcept
{
	return g_sent_sync.load(std::memory_order_relaxed);
}

uint64_t game_presents() noexcept
{
	return g_presents.load(std::memory_order_relaxed);
}

OwnPresent::OwnPresent() noexcept
{
	++t_own;
}

OwnPresent::~OwnPresent()
{
	--t_own;
}

void unhook() noexcept
{
	forget();
	std::lock_guard<std::mutex> lock(g_mutex);
	restore(g_present, reinterpret_cast<void *>(&present_detour));
	restore(g_present1, reinterpret_cast<void *>(&present1_detour));
}

}

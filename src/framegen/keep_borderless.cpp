#include "aeon_sr/framegen/keep_borderless.hpp"

#include "aeon_sr/core/diagnostics.hpp"

#include <dxgi.h>

#include <mutex>

namespace aeon_sr {

namespace {

enum class Kind : uint8_t { None, Out, In, Cover };

struct Pending {
	Kind kind = Kind::None;
	IDXGISwapChain *chain = nullptr;
	HWND hwnd = nullptr;
	ULONGLONG asked = 0;
};
std::mutex g_mutex;
Pending g_pending;
HHOOK g_hook = nullptr;
HWND g_kept = nullptr;
bool g_wants = false;
struct Tally {
	HWND hwnd = nullptr;
	uint32_t refused = 0;
	uint32_t unrun = 0;
	ULONGLONG quiet_until = 0;
};
Tally g_tally[2];
constexpr uint32_t kMaxFailures = 2;
constexpr uint32_t kMaxUnrun = 3;

Tally &tally_locked(bool in, HWND hwnd) noexcept
{
	Tally &t = g_tally[in ? 1 : 0];
	if (t.hwnd != hwnd)
		t = Tally{ hwnd, 0, 0, 0 };
	return t;
}

bool given_up(const Tally &t) noexcept
{
	return t.refused >= kMaxFailures || t.unrun >= kMaxUnrun;
}
ULONGLONG g_expiry_ms = 2000;
ULONGLONG g_cooldown_ms = 1000;
bool g_expiry_logged = false;

const GUID kReShadeSwapChainProxy = { 0x1F445F9F, 0x9887, 0x4C4C, { 0x90, 0x55, 0x4E, 0x3B, 0xAD, 0xAF, 0xCC, 0xA8 } };

IDXGISwapChain *proxy_of(IDXGISwapChain *chain) noexcept
{
	IDXGISwapChain *proxy = nullptr;
	UINT size = sizeof(proxy);
	if (FAILED(chain->GetPrivateData(kReShadeSwapChainProxy, &size, &proxy)) || size != sizeof(proxy))
		return nullptr;
	return proxy;
}

UINT switch_message() noexcept
{
	static const UINT m = RegisterWindowMessageW(L"AeonSR.KeepBorderless");
	return m;
}

void drop_pending_locked() noexcept
{
	if (g_hook != nullptr) {
		UnhookWindowsHookEx(g_hook);
		g_hook = nullptr;
	}
	if (g_pending.chain != nullptr)
		g_pending.chain->Release();
	g_pending = Pending{};
}

void expire_locked() noexcept
{
	if (g_pending.kind == Kind::None || GetTickCount64() - g_pending.asked <= g_expiry_ms)
		return;
	if (!g_expiry_logged) {
		g_expiry_logged = true;
		diag_info("framegen", L"frame generation: the game's window thread took no message within 2 s of a switch of "
			L"its fullscreen state; given up on, asked again later");
	}
	if (g_pending.kind != Kind::Cover) {
		Tally &t = tally_locked(g_pending.kind == Kind::In, g_pending.hwnd);
		t.quiet_until = GetTickCount64() + g_cooldown_ms;
		if (++t.unrun == kMaxUnrun)
			diag_info("framegen", L"frame generation: the game's window thread ran none of three switches of its "
				L"fullscreen state asked in a row; none is asked again that way for this window");
	}
	drop_pending_locked();
}

void run_pending() noexcept
{
	Pending p;
	HHOOK hook = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_pending.kind == Kind::None || GetWindowThreadProcessId(g_pending.hwnd, nullptr) != GetCurrentThreadId())
			return;
		p = g_pending;
		g_pending = Pending{};
		hook = g_hook;
		g_hook = nullptr;
	}
	if (hook != nullptr)
		UnhookWindowsHookEx(hook);
	if (p.kind == Kind::Cover) {
		make_borderless(p.hwnd);
		return;
	}
	IDXGISwapChain *const proxy = proxy_of(p.chain);
	const BOOL asked = p.kind == Kind::In ? TRUE : FALSE;
	HRESULT hr = DXGI_ERROR_INVALID_CALL;
	if (p.kind == Kind::Out || proxy != nullptr)
		hr = (proxy != nullptr ? proxy : p.chain)->SetFullscreenState(asked, nullptr);
	BOOL moved_to = asked;
	if (SUCCEEDED(hr) && SUCCEEDED(p.chain->GetFullscreenState(&moved_to, nullptr)) && moved_to != asked)
		hr = DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
	if (SUCCEEDED(hr) && p.kind == Kind::Out && proxy != nullptr)
		proxy->SetFullscreenState(TRUE, nullptr);
	if (SUCCEEDED(hr)) {
		BOOL now = p.kind == Kind::In ? TRUE : FALSE;
		BOOL real = FALSE;
		if (SUCCEEDED(p.chain->GetFullscreenState(&real, nullptr)))
			now = real;
		if (now) {
			note_game_wants_fullscreen(p.hwnd, false);
		} else if (p.kind == Kind::Out) {
			note_game_wants_fullscreen(p.hwnd, true);
			make_borderless(p.hwnd);
		}
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Tally &t = tally_locked(p.kind == Kind::In, p.hwnd);
		if (SUCCEEDED(hr)) {
			t.refused = t.unrun = 0;
		} else {
			++t.refused;
			t.quiet_until = GetTickCount64() + g_cooldown_ms;
			if (t.refused == kMaxFailures)
				diag_info("framegen", L"frame generation: the game refused a switch of its fullscreen state twice in a "
					L"row; none is asked again that way for this window");
		}
	}
	diag_logf(SUCCEEDED(hr) ? DiagLevel::Info : DiagLevel::Warn, "framegen", p.kind == Kind::In
		? L"frame generation: the game put back in the exclusive fullscreen it asked for (0x%08X)"
		: L"frame generation: the game taken out of exclusive fullscreen into a borderless window over its monitor, "
		  L"where frame generation's own window shows over it (0x%08X)", static_cast<unsigned>(hr));
	p.chain->Release();
}

LRESULT CALLBACK on_get_message(int code, WPARAM remove, LPARAM lp) noexcept
{
	if (code == HC_ACTION && remove == PM_REMOVE) {
		MSG *const m = reinterpret_cast<MSG *>(lp);
		if (m != nullptr && m->message == switch_message()) {
			m->message = WM_NULL;
			run_pending();
		}
	}
	return CallNextHookEx(nullptr, code, remove, lp);
}

bool ask_locked(const Pending &p) noexcept
{
	const DWORD thread = GetWindowThreadProcessId(p.hwnd, nullptr);
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&on_get_message), &self);
	if (thread == 0 || switch_message() == 0)
		return false;
	g_hook = SetWindowsHookExW(WH_GETMESSAGE, on_get_message, self, thread);
	if (g_hook == nullptr)
		return false;
	if (!PostMessageW(p.hwnd, switch_message(), 0, 0)) {
		UnhookWindowsHookEx(g_hook);
		g_hook = nullptr;
		return false;
	}
	if (p.chain != nullptr)
		p.chain->AddRef();
	g_pending = p;
	return true;
}

}

bool keep_borderless(const Settings &s) noexcept
{
#if AEONSR_FRAME_GENERATION
	return s.frame_generation && s.frame_generation_own_window;
#else
	(void)s;
	return false;
#endif
}

BorderlessStyle borderless_style(LONG_PTR style, LONG_PTR ex_style) noexcept
{
	BorderlessStyle b;
	b.style = (style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
		WS_MAXIMIZE | WS_MINIMIZE)) | static_cast<LONG_PTR>(WS_POPUP);
	b.ex_style = ex_style & ~static_cast<LONG_PTR>(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
		WS_EX_STATICEDGE);
	return b;
}

bool covers_monitor(HWND hwnd) noexcept
{
	if (hwnd == nullptr || !IsWindow(hwnd))
		return false;
	MONITORINFO mi{};
	mi.cbSize = sizeof(mi);
	RECT r{};
	return GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) && GetWindowRect(hwnd, &r) &&
		EqualRect(&r, &mi.rcMonitor);
}

bool should_cover(HWND hwnd) noexcept
{
	return hwnd != nullptr && IsWindow(hwnd) && IsWindowVisible(hwnd) && !IsIconic(hwnd) && !covers_monitor(hwnd);
}

bool make_borderless(HWND hwnd) noexcept
{
	if (hwnd == nullptr || !IsWindow(hwnd) || IsIconic(hwnd))
		return false;
	const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE), ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if ((style & WS_CHILD) != 0)
		return false;
	MONITORINFO mi{};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
		return false;
	const BorderlessStyle b = borderless_style(style, ex);
	RECT now{};
	GetWindowRect(hwnd, &now);
	const RECT &m = mi.rcMonitor;
	const bool own = GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId();
	const bool styled = b.style == style && b.ex_style == ex;
	if (styled && EqualRect(&now, &m))
		return false;
	if (own && !styled) {
		SetWindowLongPtrW(hwnd, GWL_STYLE, b.style);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, b.ex_style);
	}
	SetWindowPos(hwnd, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top,
		SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | (own && !styled ? SWP_FRAMECHANGED : 0u));
	if (!own && !styled)
		cover_monitor_later(hwnd);
	diag_logf(DiagLevel::Info, "framegen", L"frame generation: the game's window made borderless over its monitor, "
		L"%ldx%ld at %ld,%ld (was %ldx%ld)%ls", m.right - m.left, m.bottom - m.top, m.left, m.top, now.right - now.left,
		now.bottom - now.top, own || styled ? L"" : L", its styles asked of its own thread");
	return true;
}

void note_game_wants_fullscreen(HWND hwnd, bool fullscreen) noexcept
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (fullscreen) {
		g_kept = hwnd;
		g_wants = true;
	} else if (hwnd == g_kept) {
		g_wants = false;
	}
}

bool game_wants_fullscreen(HWND hwnd) noexcept
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_wants && hwnd == g_kept;
}

bool refuse_fullscreen_request(HWND hwnd, bool dxgi, bool fullscreen, bool keep, bool init_notice) noexcept
{
	if (init_notice || !dxgi)
		return false;
	if (!fullscreen) {
		note_game_wants_fullscreen(hwnd, false);
		return false;
	}
	if (!keep)
		return false;
	note_game_wants_fullscreen(hwnd, true);
	return true;
}

void tell_game_fullscreen(IDXGISwapChain *chain) noexcept
{
	if (IDXGISwapChain *const proxy = chain != nullptr ? proxy_of(chain) : nullptr)
		proxy->SetFullscreenState(TRUE, nullptr);
}

bool switch_fullscreen_later(IDXGISwapChain *chain, HWND hwnd, bool fullscreen) noexcept
{
	if (chain == nullptr || hwnd == nullptr || !IsWindow(hwnd))
		return false;
	const Kind kind = fullscreen ? Kind::In : Kind::Out;
	std::lock_guard<std::mutex> lock(g_mutex);
	expire_locked();
	if (g_pending.kind != Kind::None) {
		if (g_pending.kind == kind && g_pending.hwnd == hwnd)
			return true;
		drop_pending_locked();
	}
	const Tally &t = tally_locked(fullscreen, hwnd);
	if (given_up(t) || GetTickCount64() < t.quiet_until)
		return false;
	return ask_locked(Pending{ kind, chain, hwnd, GetTickCount64() });
}

bool cover_monitor_later(HWND hwnd) noexcept
{
	if (hwnd == nullptr || !IsWindow(hwnd))
		return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	expire_locked();
	if (g_pending.kind != Kind::None)
		return g_pending.kind == Kind::Cover && g_pending.hwnd == hwnd;
	return ask_locked(Pending{ Kind::Cover, nullptr, hwnd, GetTickCount64() });
}

bool switch_pending(bool fullscreen) noexcept
{
	std::lock_guard<std::mutex> lock(g_mutex);
	expire_locked();
	return g_pending.kind == (fullscreen ? Kind::In : Kind::Out);
}

void cancel_switch(HWND hwnd) noexcept
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_pending.kind != Kind::None && (hwnd == nullptr || g_pending.hwnd == hwnd))
		drop_pending_locked();
}

uint32_t switch_failures(bool fullscreen) noexcept
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_tally[fullscreen ? 1 : 0].refused;
}

bool switch_given_up(HWND hwnd, bool fullscreen) noexcept
{
	std::lock_guard<std::mutex> lock(g_mutex);
	const Tally &t = g_tally[fullscreen ? 1 : 0];
	return t.hwnd == hwnd && given_up(t);
}

FollowStep follow_step(bool keep, bool pending_out, bool pending_in, bool wants, int fullscreen) noexcept
{
	FollowStep f;
	f.cancel = keep ? pending_in : pending_out;
	if (!wants)
		return f;
	if (keep) {
		f.cover = !pending_out;
		return f;
	}
	if (fullscreen > 0)
		f.forget = true;
	else if (fullscreen == 0)
		f.put_back = true;
	return f;
}

bool keep_taking_out(bool leave_exclusive, bool given_up) noexcept
{
	return leave_exclusive && !given_up;
}

bool has_reshade_proxy(IDXGISwapChain *chain) noexcept
{
	return chain != nullptr && proxy_of(chain) != nullptr;
}

void set_switch_timing(ULONGLONG expiry_ms, ULONGLONG cooldown_ms) noexcept
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_expiry_ms = expiry_ms;
	g_cooldown_ms = cooldown_ms;
	g_tally[0].quiet_until = g_tally[1].quiet_until = 0;
}

}

#pragma once

#include "aeon_sr/core/settings.hpp"

#include <Windows.h>

#include <cstdint>

struct IDXGISwapChain;

namespace aeon_sr {

bool keep_borderless(const Settings &s) noexcept;

struct BorderlessStyle {
	LONG_PTR style;
	LONG_PTR ex_style;
};
BorderlessStyle borderless_style(LONG_PTR style, LONG_PTR ex_style) noexcept;

bool make_borderless(HWND hwnd) noexcept;
bool covers_monitor(HWND hwnd) noexcept;
bool should_cover(HWND hwnd) noexcept;

void note_game_wants_fullscreen(HWND hwnd, bool fullscreen) noexcept;
bool game_wants_fullscreen(HWND hwnd) noexcept;

bool refuse_fullscreen_request(HWND hwnd, bool dxgi, bool fullscreen, bool keep, bool init_notice) noexcept;

class SwapchainNotices {
public:
	void on_create() noexcept { made_windowed_ = false; }
	void on_made_windowed() noexcept { made_windowed_ = true; }
	bool on_init(bool dxgi_with_proxy, bool resize) noexcept
	{
		const bool tell = made_windowed_ && dxgi_with_proxy && !resize;
		made_windowed_ = false;
		notice_ = false;
		return tell;
	}
	void expect_notice(bool dxgi_with_proxy) noexcept { notice_ = dxgi_with_proxy; }
	bool take_notice() noexcept
	{
		const bool n = notice_;
		notice_ = false;
		return n;
	}

private:
	bool made_windowed_ = false;
	bool notice_ = false;
};

bool has_reshade_proxy(IDXGISwapChain *chain) noexcept;

void tell_game_fullscreen(IDXGISwapChain *chain) noexcept;

bool switch_fullscreen_later(IDXGISwapChain *chain, HWND hwnd, bool fullscreen) noexcept;
bool cover_monitor_later(HWND hwnd) noexcept;
bool switch_pending(bool fullscreen) noexcept;
void cancel_switch(HWND hwnd) noexcept;
uint32_t switch_failures(bool fullscreen) noexcept;
bool switch_given_up(HWND hwnd, bool fullscreen) noexcept;

struct FollowStep {
	bool cancel = false;
	bool cover = false;
	bool forget = false;
	bool put_back = false;
};
FollowStep follow_step(bool keep, bool pending_out, bool pending_in, bool wants, int fullscreen) noexcept;
bool keep_taking_out(bool leave_exclusive, bool given_up) noexcept;
void set_switch_timing(ULONGLONG expiry_ms, ULONGLONG cooldown_ms) noexcept;

}

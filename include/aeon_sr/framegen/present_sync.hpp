#pragma once

#include <cstdint>

namespace aeon_sr::present_sync {

constexpr uint32_t kUnknown = 0xFFFFFFFFu;

bool watch(void *native_swapchain) noexcept;
void forget() noexcept;
void force_vsync(bool on) noexcept;
uint32_t game_sync_interval() noexcept;
uint64_t game_presents() noexcept;
uint32_t sent_sync_interval() noexcept;

struct OwnPresent {
	OwnPresent() noexcept;
	~OwnPresent();
	OwnPresent(const OwnPresent &) = delete;
	OwnPresent &operator=(const OwnPresent &) = delete;
};

void unhook() noexcept;

void apply(bool forcing, uint32_t *sync_interval, uint32_t *flags) noexcept;

}

#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aeon_sr {

class PresentDrive {
public:
	void effects_event() noexcept { effects_seen_ = true; }
	bool at_present(bool wanted, bool effects_on) noexcept
	{
		const bool effects_ran = effects_seen_;
		effects_seen_ = false;
		drove_ = wanted && effects_on && !effects_ran;
		if (drove_)
			++driven_;
		return drove_;
	}
	bool frame_taken() const noexcept { return drove_; }
	uint64_t driven() const noexcept { return driven_; }

private:
	bool effects_seen_ = false;
	bool drove_ = false;
	uint64_t driven_ = 0;
};

bool effect_paths_reach(const std::vector<std::string> &paths, const std::string &addon_dir,
	const std::string &base_dir);

bool d3d9_device_lost(reshade::api::device *device) noexcept;

class GameStateGuard {
public:
	GameStateGuard(reshade::api::device *device, reshade::api::command_list *cmd_list) noexcept;
	~GameStateGuard();
	GameStateGuard(const GameStateGuard &) = delete;
	GameStateGuard &operator=(const GameStateGuard &) = delete;

	static void release_cached() noexcept;

private:
	reshade::api::device_api api_ = reshade::api::device_api::d3d12;
	void *saved_ = nullptr;
	void *context_ = nullptr;
	void *targets_[5] = {};
};

}

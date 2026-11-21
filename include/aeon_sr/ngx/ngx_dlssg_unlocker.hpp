#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aeon_sr {

struct PeHeader {
	bool pe = false;
	uint16_t machine = 0;
	bool dll = false;
};

PeHeader pe_header_of(const uint8_t *bytes, size_t size) noexcept;
PeHeader pe_header_of_file(const std::wstring &path);
std::wstring pe_machine_label(uint16_t machine);

std::wstring dlssg_unlocker_family(const std::wstring &file_name);

int dlssg_unlocker_proxy_rank(const std::wstring &file_name);

struct UnlockerCandidate {
	std::wstring path;
	std::wstring family;
	PeHeader header;
	bool usable = false;
	std::wstring why;
	std::wstring brief;
};

struct UnlockerPlan {
	std::vector<UnlockerCandidate> found;
	int chosen = -1;
	bool disabled = false;
	std::wstring setting_path;
	std::wstring searched;
};

UnlockerPlan plan_dlssg_unlocker(const std::vector<std::wstring> &dirs, const std::wstring &setting,
	const std::wstring &base_dir, uint16_t want, const std::wstring &windows_dir);

std::wstring resolve_unlocker_setting(const std::wstring &setting, const std::wstring &base_dir);

std::wstring read_unlocker_setting(const std::wstring &ini_path);

enum class UnlockerMode : uint8_t {
	Report,
	Load,
};

struct UnlockerConfig {
	UnlockerMode mode = UnlockerMode::Report;
	std::wstring game_dir;
	std::wstring addon_dir;
	bool setting_given = false;
	std::wstring setting;
};

void configure_dlssg_unlocker(const UnlockerConfig &config);

std::wstring prepare_dlssg_unlocker(const std::wstring &addon_dir);

std::wstring dlssg_unlocker_note();

void reset_dlssg_unlocker_for_tests();

}

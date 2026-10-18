#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aeon_sr {

class RecentLook {
public:
	static constexpr uint64_t kHoldMs = 1000;
	bool fresh(uint64_t now_ms) const noexcept { return looked_ && now_ms - at_ms_ < kHoldMs; }
	bool found() const noexcept { return found_; }
	uint32_t walks() const noexcept { return walks_; }
	void note(bool found, uint64_t now_ms) noexcept
	{
		looked_ = true;
		found_ = found;
		at_ms_ = now_ms;
		++walks_;
	}

private:
	bool looked_ = false;
	bool found_ = false;
	uint64_t at_ms_ = 0;
	uint32_t walks_ = 0;
};

inline constexpr const wchar_t *kFsrPreferredSubdir = L"fsr4-int8";

inline constexpr const wchar_t *kNeuralPreferredSubdir = L"dlss5-allgpu";

std::wstring path_under(const std::wstring &dir, const wchar_t *file_name);

bool optiscaler_folder(const std::wstring &dir);

std::vector<std::wstring> demote_dirs(std::vector<std::wstring> dirs,
	const std::function<bool(const std::wstring &)> &foreign);

std::vector<std::wstring> runtime_search_dirs(const std::wstring &addon_dir,
	const std::wstring &exe_dir);

std::vector<std::wstring> preferred_search_dirs(const std::wstring &addon_dir,
	const std::wstring &exe_dir, const wchar_t *preferred_subdir);

std::vector<std::wstring> fsr_search_dirs(const std::wstring &addon_dir,
	const std::wstring &exe_dir);

std::vector<std::wstring> runtime_candidates(const std::vector<std::wstring> &dirs,
	const wchar_t *file_name);

}

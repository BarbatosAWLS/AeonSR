#pragma once

#include <string>
#include <vector>

namespace aeon_sr {

inline constexpr const wchar_t *kFsrPreferredSubdir = L"fsr4-int8";

inline constexpr const wchar_t *kNeuralPreferredSubdir = L"dlss5-allgpu";

std::wstring path_under(const std::wstring &dir, const wchar_t *file_name);

std::vector<std::wstring> runtime_search_dirs(const std::wstring &addon_dir,
	const std::wstring &exe_dir);

std::vector<std::wstring> preferred_search_dirs(const std::wstring &addon_dir,
	const std::wstring &exe_dir, const wchar_t *preferred_subdir);

std::vector<std::wstring> fsr_search_dirs(const std::wstring &addon_dir,
	const std::wstring &exe_dir);

std::vector<std::wstring> runtime_candidates(const std::vector<std::wstring> &dirs,
	const wchar_t *file_name);

}

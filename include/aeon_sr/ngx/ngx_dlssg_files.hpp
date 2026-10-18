#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aeon_sr {

enum class DlssgSource : uint8_t {
	None = 0,
	Loaded,
	GameFolder,
	AddonRuntime,
};

const wchar_t *dlssg_source_label(DlssgSource s) noexcept;

struct DlssgChoice {
	DlssgSource source = DlssgSource::None;
	std::wstring file;
	std::wstring dir;
	std::wstring why;
	std::wstring addon_file;
};

DlssgChoice choose_dlssg(const std::vector<std::wstring> &loaded, const std::wstring &exe_dir,
	const std::wstring &addon_file, const std::function<bool(const std::wstring &)> &exists);

struct NgxPathList {
	std::vector<std::wstring> dirs;
	std::wstring note;
};

NgxPathList ngx_shared_path_list(const std::wstring &dlss_dir, const DlssgChoice &g,
	const std::function<bool(const std::wstring &)> &exists,
	const std::function<uint64_t(const std::wstring &)> &version_of);

std::wstring first_holding(const std::vector<std::wstring> &dirs, const wchar_t *file_name,
	const std::function<bool(const std::wstring &)> &exists);

uint64_t file_version_number(const std::wstring &path);
std::wstring version_text(uint64_t v);

DlssgChoice find_dlssg(const std::wstring &addon_dir);
NgxPathList ngx_path_list_for(const std::wstring &dlss_dir, const DlssgChoice &g);

std::wstring describe_ngx_modules();

}

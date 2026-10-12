#pragma once

#include <string>

namespace aeon_sr {

std::wstring ngx_override_path_from(const std::wstring &env, const std::wstring &exe_dir,
	const std::wstring &addon_dir, bool under_wine);

const std::wstring &ngx_override_path(const std::wstring &addon_dir);

bool running_under_wine() noexcept;

std::wstring ngx_override_dlss_dir(const std::wstring &override_path);

bool ngx_same_file(const std::wstring &a, const std::wstring &b);

}

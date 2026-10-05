#pragma once

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/dxgi_format_util.hpp"

#include <nvsdk_ngx_defs.h>

#include <string>

namespace aeon_sr {

void NVSDK_CONV ngx_log_callback(const char *msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature);

const wchar_t *ngx_result_name(NVSDK_NGX_Result r);
std::wstring ngx_format_result(const wchar_t *what, NVSDK_NGX_Result r);

bool file_exists_w(const std::wstring &path);
std::wstring exe_directory_w();
std::wstring local_appdata_dir_w();
std::wstring ngx_dll_path(const std::wstring &dir);

extern const char kBlitHlsl[];

}

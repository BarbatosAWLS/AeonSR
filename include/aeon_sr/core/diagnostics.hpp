#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aeon_sr {

enum class DiagLevel : unsigned int {
	Trace = 0,
	Info = 1,
	Warn = 2,
	Error = 3,
};

const char *diag_level_label(DiagLevel level) noexcept;

struct DiagEntry {
	float time_s = 0.0f;
	DiagLevel level = DiagLevel::Info;
	std::string tag;
	std::wstring text;
};

void diag_open_log(const std::wstring &path);
std::wstring diag_log_path();
void diag_close_log();

void diag_set_verbose(bool on) noexcept;
bool diag_verbose() noexcept;

void diag_set_sink(void (*sink)(DiagLevel, const char *)) noexcept;

void diag_log(DiagLevel level, const char *tag, const std::wstring &text);
void diag_logf(DiagLevel level, const char *tag, const wchar_t *fmt, ...);

void diag_state(const char *key, DiagLevel level, const char *tag, const std::wstring &text);

inline void diag_trace(const char *tag, const std::wstring &text) { diag_log(DiagLevel::Trace, tag, text); }
inline void diag_info(const char *tag, const std::wstring &text) { diag_log(DiagLevel::Info, tag, text); }
inline void diag_warn(const char *tag, const std::wstring &text) { diag_log(DiagLevel::Warn, tag, text); }
inline void diag_error(const char *tag, const std::wstring &text) { diag_log(DiagLevel::Error, tag, text); }

inline constexpr size_t kDiagHistory = 256;
std::vector<DiagEntry> diag_recent(size_t max_entries, DiagLevel min_level);

uint32_t diag_count(DiagLevel level) noexcept;
std::wstring diag_last_line();
std::wstring diag_last_error();

std::string diag_narrow(const std::wstring &text);
std::wstring diag_widen(const char *text);

struct DiagAdapter {
	std::wstring name;
	uint32_t vendor_id = 0, device_id = 0, subsys_id = 0, revision = 0;
	uint64_t dedicated_vram = 0, shared_memory = 0;
	std::wstring driver_version;
	uint32_t luid_low = 0;
	int32_t luid_high = 0;
	bool active = false;
};

struct DiagFile {
	const wchar_t *label = nullptr;
	std::wstring path;
	bool present = false;
	uint64_t size = 0;
	std::wstring version;
	std::wstring modified;
};

struct DiagEnvironment {
	std::wstring addon_version;
	std::wstring build_stamp;
	std::wstring addon_dir;
	std::wstring addon_path;
	std::wstring exe_path;
	std::wstring os_version;
	std::wstring reshade_version;
	std::wstring reshade_path;
	uint32_t nv_driver_major = 0, nv_driver_minor = 0;
	uint32_t nv_architecture = 0;
	std::vector<DiagAdapter> adapters;
	std::vector<DiagFile> files;
	std::wstring foreign_modules;
	bool collected = false;
	bool gpu_collected = false;
};

uint32_t nvidia_architecture_id();

bool nvidia_driver_version(uint32_t luid_low, int32_t luid_high,
	uint32_t *out_major, uint32_t *out_minor);

void diag_collect_environment(void *addon_module);

void diag_collect_environment_local(void *addon_module);

void diag_collect_environment_gpu();

const DiagEnvironment &diag_environment();
void diag_log_environment();
void diag_log_environment_gpu();

}

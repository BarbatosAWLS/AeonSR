#include "aeon_sr/core/diag_report.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/gpu_vendor.hpp"
#include "aeon_sr/core/i18n.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/ngx/ngx_dlssnr.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <Windows.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace aeon_sr {
namespace {

std::wstring wformat(const wchar_t *fmt, ...)
{
	wchar_t buf[1024]{};
	va_list args;
	va_start(args, fmt);
	_vsnwprintf_s(buf, _TRUNCATE, fmt, args);
	va_end(args);
	return buf;
}

std::string format(const char *fmt, ...)
{
	char buf[2048]{};
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return buf;
}

const char *bool_label(bool v) noexcept { return v ? "yes" : "no"; }

const char *motion_text(MotionProvider p, i18n::Lang lang) noexcept
{
	return i18n::text(lang, p == MotionProvider::Internal ? i18n::S::hd_motion_builtin : i18n::S::hd_none);
}

const char *depth_text(DepthProvider p, i18n::Lang lang) noexcept
{
	switch (p) {
	case DepthProvider::Generic: return i18n::text(lang, i18n::S::hd_depth_raw);
	case DepthProvider::Normalized: return i18n::text(lang, i18n::S::hd_depth_normalised);
	default: return i18n::text(lang, i18n::S::hd_none);
	}
}

void append_settings(std::wstring &out, const Settings &s)
{
	out += L"SETTINGS\n";
	const std::wstring backend = s.backend < kBackendChoiceCount
		? wformat(L"%u", s.backend)
		: std::wstring(L"unset (settled on the first frame)");
	out += wformat(L"  enabled=%hs  backend=%s  mode=%u  preset=%u  sharpness=%.2f\n",
		bool_label(s.enabled), backend.c_str(), s.upscale_mode, s.render_preset,
		static_cast<double>(s.sharpness));
	out += wformat(L"  upscale_effects=%hs  jitter=%hs  mv_probe=%hs\n",
		bool_label(s.upscale_effects), bool_label(s.spatial_jitter), bool_label(s.mv_probe));
	out += wformat(L"  vsr_input_format=%u  fsr_provider=%hs  optiscaler_upscale=%hs\n", s.vsr_input_format,
		s.fsr_provider.empty() ? "auto" : s.fsr_provider.c_str(), bool_label(s.optiscaler_upscale));
	static const wchar_t *const kModeNames[kNeuralModeCount] = {
		L"quality", L"balanced", L"performance", L"ultra performance" };
	const std::wstring mode = kModeNames[clamp_neural_mode(s.neural_mode)];
	out += wformat(L"  neural=%hs  style=%u  model_scale=%u%%  passes=%u  mode=%s\n",
		bool_label(s.neural_render), s.neural_style,
		neural_model_scale_percent(s.neural_model_scale), s.neural_passes, mode.c_str());
	out += wformat(L"  neural detail=%.2f  colour=%.2f  intensity=%.2f  structure=%.2f  tone=%.2f  skin=%.2f\n",
		static_cast<double>(s.neural_detail_strength), static_cast<double>(s.neural_colour_strength),
		static_cast<double>(s.neural_intensity), static_cast<double>(s.neural_local_structure),
		static_cast<double>(s.neural_local_tone), static_cast<double>(s.neural_skin_structure));
	out += wformat(L"  neural smoothing=%hs (blend=%.2f steadiness=%.2f reject=%.2f)  "
		L"auto_mask=%hs  reset_on_cut=%hs\n",
		bool_label(s.neural_smoothing),
		static_cast<double>(s.neural_smooth_blend), static_cast<double>(s.neural_smooth_steady),
		static_cast<double>(s.neural_carry_reject), bool_label(s.neural_auto_mask),
		bool_label(s.neural_reset_on_cut));
	out += wformat(L"  neural paper_white=%.2f  highlight_guard=%hs (%.2f)  "
		L"no_depth=%hs  no_motion_vectors=%hs\n",
		static_cast<double>(s.neural_paper_white),
		bool_label(s.neural_highlight_guard_on),
		static_cast<double>(s.neural_highlight_guard),
		bool_label(s.neural_no_depth), bool_label(s.neural_no_motion_vectors));
	out += wformat(L"  debug_view=%u  neural_debug_view=%u  osd=%hs  debug_info=%hs  verbose_log=%hs\n",
		s.debug_view, s.neural_debug_view, bool_label(s.show_osd), bool_label(s.debug_info),
		bool_label(s.verbose_log));
}

}

const char *check_state_label(CheckState s) noexcept
{
	switch (s) {
	case CheckState::Ok: return " OK ";
	case CheckState::Warn: return "WARN";
	case CheckState::Fail: return "FAIL";
	case CheckState::Off: return "off ";
	}
	return "    ";
}

const char *diag_status_text(UpscalerStatus status, i18n::Lang lang) noexcept
{
	using i18n::S;
	S id = S::up_unknown;
	switch (status) {
	case UpscalerStatus::Idle: id = S::up_idle; break;
	case UpscalerStatus::MissingRuntime: id = S::up_missing_runtime; break;
	case UpscalerStatus::UnsupportedApi: id = S::up_unsupported_api; break;
	case UpscalerStatus::UnsupportedGpu: id = S::up_unsupported_gpu; break;
	case UpscalerStatus::InitFailed: id = S::up_init_failed; break;
	case UpscalerStatus::NeedMotionVectors: id = S::up_need_motion; break;
	case UpscalerStatus::NeedDepth: id = S::up_need_depth; break;
	case UpscalerStatus::Ready: id = S::up_running; break;
	case UpscalerStatus::EvaluateFailed: id = S::up_evaluate_failed; break;
	case UpscalerStatus::Crashed: id = S::up_crashed; break;
	case UpscalerStatus::Loading: id = S::up_loading; break;
	}
	return i18n::text(lang, id);
}

CheckState diag_worst(const std::vector<DiagCheck> &checks) noexcept
{
	CheckState worst = CheckState::Off;
	for (const DiagCheck &c : checks)
		if (static_cast<unsigned>(c.state) > static_cast<unsigned>(worst))
			worst = c.state;
	return worst;
}

std::vector<DiagCheck> diag_checks(const PanelState &state, const Settings &settings, i18n::Lang lang)
{
	using i18n::S;
	const DiagEnvironment &e = diag_environment();
	std::vector<DiagCheck> out;
	const auto T = [lang](S id) { return i18n::text(lang, id); };
	std::string said_tech;
	const auto said = [lang, &said_tech](const std::wstring &english, S fallback) {
		std::string tech;
		std::string out_text = diag_said(diag_narrow(english), lang, fallback, &tech);
		if (!tech.empty())
			said_tech += (said_tech.empty() ? "" : "\n") + tech;
		return out_text;
	};
	auto add = [&out, &said_tech, lang](S name, CheckState s, std::string detail, std::string action = {}) {
		DiagCheck c;
		c.name = i18n::english(name);
		c.title = i18n::text(lang, name);
		c.state = s;
		c.detail = std::move(detail);
		c.action = std::move(action);
		c.tech = std::move(said_tech);
		said_tech.clear();
		out.push_back(std::move(c));
	};

	add(S::hn_api, CheckState::Ok, state.api_name != nullptr ? state.api_name : T(S::hd_unknown));
	if (state.bridge_name != nullptr) {
		said_tech = state.bridge_name;
		if (state.bridge_sync != nullptr)
			said_tech += std::string(", ") + state.bridge_sync;
		add(S::hn_bridge, state.bridge_any_ready ? CheckState::Ok : CheckState::Warn,
			T(state.bridge_any_ready ? S::hd_bridge_ready : S::hd_bridge_waiting),
			state.bridge_any_ready ? "" : T(S::ha_bridge_not_yet));
	}
	else if (!state.bridge_error.empty())
		add(S::hn_bridge, CheckState::Fail, T(S::hd_none),
			T(S::ha_bridge_failed));

	if (state.upscalers_remote && state.host_ready)
		add(S::hn_where, CheckState::Ok,
			format(T(S::hd_host_process), state.host_pid),
			T(S::ha_host_32bit));
	else if (state.upscalers_remote)
		add(S::hn_where, CheckState::Off,
			T(S::hd_host_not_started),
			T(S::ha_host_not_started));

	if (state.gpu_name.empty()) {
		add(S::hn_gpu, CheckState::Warn, T(S::hd_gpu_unknown),
			T(S::ha_gpu_unknown));
	} else {
		std::string gpu = diag_narrow(state.gpu_name);
		if (e.nv_driver_major != 0)
			gpu += format(T(S::hd_gpu_driver), e.nv_driver_major, e.nv_driver_minor);
		add(S::hn_gpu, CheckState::Ok, std::move(gpu));
	}

	if (!e.reshade_version.empty())
		add(S::hn_reshade, CheckState::Ok, diag_narrow(e.reshade_version));

	if (!state.motion_wanted) {
		add(S::hn_motion, CheckState::Off,
			T(S::hd_motion_not_needed));
	} else if (state.motion_provider == MotionProvider::None && state.flow_note_benign) {
		add(S::hn_motion, CheckState::Warn, said(state.flow_note, S::hd_motion_stopped),
			T(S::ha_motion_benign));
	} else if (state.motion_provider == MotionProvider::None) {
		add(S::hn_motion, CheckState::Fail,
			state.flow_note.empty() ? T(S::hd_motion_none) : said(state.flow_note, S::hd_motion_stopped),
			T(S::ha_motion_none));
	} else if (state.probe_valid && state.probe_nonzero_pct <= 0.0f && state.probe_moving) {
		add(S::hn_motion, CheckState::Fail, T(S::hd_motion_zero),
			T(S::ha_motion_zero));
	} else if (state.probe_valid) {
		add(S::hn_motion, CheckState::Ok,
			format(T(S::hd_motion_probe),
				motion_text(state.motion_provider, lang),
				static_cast<double>(state.probe_nonzero_pct),
				static_cast<double>(state.probe_mean_px)));
	} else {
		add(S::hn_motion, CheckState::Ok, motion_text(state.motion_provider, lang));
	}

	if (state.depth_provider == DepthProvider::None) {
		add(S::hn_depth, CheckState::Warn, T(S::hd_depth_none),
			T(S::ha_depth_none));
	} else {
		std::string depth_line = depth_text(state.depth_provider, lang);
		if (state.depth_reversed)
			depth_line += std::string(T(S::list_sep)) + T(S::hd_depth_reversed);
		if (state.depth_upside_down)
			depth_line += std::string(T(S::list_sep)) + T(S::hd_depth_upside_down);
		if (state.depth_mirrored)
			depth_line += std::string(T(S::list_sep)) + T(S::hd_depth_mirrored);
		if (state.depth_far_plane > 0.0f)
			depth_line += std::string(T(S::list_sep)) + format(T(S::hd_depth_far), static_cast<double>(state.depth_far_plane));
		if (state.depth_logarithmic)
			depth_line += std::string(T(S::list_sep)) + T(S::hd_depth_log);
		if (state.depth_overridden.empty())
			add(S::hn_depth, CheckState::Ok, depth_line);
		else
			add(S::hn_depth, CheckState::Warn, depth_line,
				format(T(S::ha_depth_overridden), state.depth_overridden.c_str()));
	}

	const bool no_upscaler = !settings.enabled ||
		settings.backend == static_cast<unsigned>(BackendChoice::None);
	if (no_upscaler) {
		add(S::hn_upscaler, CheckState::Off, T(S::hd_upscaler_off));
	} else {
		const std::string name = state.optiscaler_upscales && state.active_is_dlss
			? std::string(T(S::hd_upscaler_optiscaler))
			: !state.upscaler_label.empty() ? state.upscaler_label
			: std::string(state.active_name != nullptr ? state.active_name : T(S::hd_unknown));
		std::string detail = format(T(S::hd_upscaler_status), name.c_str(), diag_status_text(state.active_status, lang));
		std::string action;
		CheckState s = CheckState::Ok;
		switch (state.active_status) {
		case UpscalerStatus::Ready: {
			const bool vendor = state.active_is_fsr || state.active_is_xess || state.active_is_vsr;
			const uint32_t rw = vendor ? state.upscaler_render_width : state.ngx_render_width;
			const uint32_t rh = vendor ? state.upscaler_render_height : state.ngx_render_height;
			const uint32_t ow = vendor ? state.upscaler_out_width : state.ngx_width;
			const uint32_t oh = vendor ? state.upscaler_out_height : state.ngx_height;
			detail += rw == ow && rh == oh ? format(T(S::hd_upscaler_at), ow, oh)
				: format(T(S::hd_upscaler_sizes), rw, rh, ow, oh);
			if (state.optiscaler_upscales)
				action = T(S::ha_optiscaler_upscales);
			break;
		}
		case UpscalerStatus::MissingRuntime:
			s = CheckState::Fail;
			action = format(T(S::ha_missing_runtime), kAddonFileName);
			break;
		case UpscalerStatus::UnsupportedApi:
			s = CheckState::Fail;
			if (state.active_error.empty())
				action = T(S::ha_unsupported_api);
			break;
		case UpscalerStatus::UnsupportedGpu:
			s = CheckState::Fail;
			action = T(S::ha_unsupported_gpu);
			break;
		case UpscalerStatus::NeedMotionVectors:
			s = CheckState::Warn;
			break;
		case UpscalerStatus::NeedDepth:
			s = CheckState::Warn;
			action = T(S::ha_need_depth);
			break;
		case UpscalerStatus::Crashed:
			s = CheckState::Fail;
			action = T(S::ha_crashed);
			break;
		case UpscalerStatus::InitFailed:
			s = CheckState::Fail;
			action = T(S::ha_init_failed);
			break;
		case UpscalerStatus::EvaluateFailed:
			s = CheckState::Fail;
			action = T(S::ha_evaluate_failed);
			break;
		case UpscalerStatus::Loading:
			s = CheckState::Warn;
			action = T(S::ha_loading);
			break;
		case UpscalerStatus::Idle:
			s = CheckState::Warn;
			action = T(S::ha_idle);
			break;
		}
		if (!state.active_error.empty()) {
			const std::string why = said(state.active_error, S::Count);
			if (!why.empty())
				detail += " - " + why;
		}
		add(S::hn_upscaler, s, std::move(detail), std::move(action));
	}

	if (!state.ngx_layer.empty()) {
		if (state.ngx_layer_relevant && !state.host_error.empty())
		{
			const std::string why = said(state.host_error, S::Count);
			add(S::hn_other_program, CheckState::Fail, format(T(S::hd_loaded_in_game), diag_narrow(state.ngx_layer).c_str()),
				why.empty() ? std::string(T(S::ha_other_failed_short)) : format(T(S::ha_other_failed), why.c_str()));
		}
		else {
			std::string action;
			if (state.optiscaler_upscales)
				action = T(S::ha_other_takes_upscale);
			else if (state.optiscaler_held == PanelState::OptiscalerHeld::PickDlss)
				action = T(S::ha_other_pick_dlss);
			else if (state.optiscaler_held == PanelState::OptiscalerHeld::GameDlss)
				action = T(S::ha_other_game_dlss);
			else if (!state.upscalers_remote)
				action = state.ngx_layer_relevant
					? T(S::ha_other_runs)
					: T(S::ha_other_would_run);
			add(S::hn_other_program, CheckState::Ok, format(T(S::hd_loaded_in_game), diag_narrow(state.ngx_layer).c_str()),
				std::move(action));
		}
	}

	if (state.native_dlss) {
		const bool dlss_on = settings.enabled && settings.backend == static_cast<unsigned>(BackendChoice::Dlss);
		if (dlss_on && !state.host_ready && !state.host_error.empty())
		{
			const std::string why = said(state.host_error, S::Count);
			add(S::hn_game_dlss, CheckState::Fail, format(T(S::hd_loaded_by_game), diag_narrow(state.native_dlss_modules).c_str()),
				why.empty() ? std::string(T(S::ha_game_dlss_failed_short)) : format(T(S::ha_game_dlss_failed), why.c_str()));
		}
		else {
			std::string action;
			if (state.ngx_layer.empty() && !state.upscalers_remote)
				action = dlss_on ? T(S::ha_game_dlss_runs)
					: T(S::ha_game_dlss_would_run);
			add(S::hn_game_dlss, CheckState::Ok, format(T(S::hd_loaded_by_game), diag_narrow(state.native_dlss_modules).c_str()),
				std::move(action));
		}
	}

	if (!settings.neural_render) {
		add(S::hn_neural, CheckState::Off, T(S::hd_turned_off));
	} else if (state.neural_last == 1) {
		const unsigned int asked = clamp_neural_passes(settings.neural_passes);
		const bool short_chain = state.neural_passes_built != 0 && state.neural_passes_built < asked;
		std::string detail = short_chain
			? format(T(S::hd_running_passes), state.neural_passes_built, asked)
			: std::string(T(S::hd_running));
		if (state.neural_reported_architecture != 0u)
			detail += T(S::hd_card_check_bypassed);
		add(S::hn_neural, short_chain ? CheckState::Warn : CheckState::Ok, std::move(detail),
			short_chain ? T(S::ha_short_chain) : "");
	} else if (!vendor_may_be_nvidia(state.gpu_vendor)) {
		add(S::hn_neural, CheckState::Fail,
			format(T(S::hd_needs_nvidia), state.gpu_vendor == GpuVendor::Unknown ? T(S::hd_unknown) : vendor_label(state.gpu_vendor)),
			T(S::ha_everything_else_works));
	} else if (state.neural_host_refused) {
		add(S::hn_neural, CheckState::Fail, T(S::hd_host_refused),
			T(S::ha_host_refused));
	} else if (!state.neural_dll_present) {
		add(S::hn_neural, CheckState::Fail, T(S::hd_dlssnr_missing),
			T(S::ha_dlssnr_missing));
	} else if (!state.neural_shim_present) {
		add(S::hn_neural, CheckState::Fail, T(S::hd_shim_missing),
			format(T(S::ha_shim_missing), kAddonFileName));
	} else if (!state.neural_runtime_serves_card) {
		add(S::hn_neural, CheckState::Fail,
			state.neural_runtime_cards.empty()
				? std::string(T(S::hd_runtime_no_code))
				: format(T(S::hd_runtime_built_for), diag_cards_text(state.neural_runtime_cards, lang).c_str()),
			state.neural_allgpu_build_targets
				? format(T(S::ha_allgpu_build), diag_arch_text(state.neural_gpu_architecture, lang))
				: format(T(S::ha_no_build), diag_arch_text(state.neural_gpu_architecture, lang)));
	} else if (state.neural_adapter_unsupported) {
		add(S::hn_neural, CheckState::Fail,
			format(T(S::hd_needs_arch), diag_arch_text(state.neural_min_architecture, lang)),
			T(S::ha_everything_else_works));
	} else if (state.neural_driver_too_old) {
		add(S::hn_neural, CheckState::Fail,
			format(T(S::hd_driver_old),
				state.neural_driver_major, state.neural_driver_minor,
				state.neural_required_driver_major, state.neural_required_driver_minor),
			T(S::ha_update_driver));
	} else if (state.neural_feature_unsupported) {
		add(S::hn_neural, CheckState::Fail, T(S::hd_feature_refused),
			T(S::ha_not_retryable));
	} else if (state.neural_crashed) {
		add(S::hn_neural, CheckState::Fail, T(S::hd_neural_faulted),
			T(S::ha_neural_faulted));
	} else if (state.neural_last == -1) {
		add(S::hn_neural, CheckState::Fail,
			state.neural_error.empty() ? std::string(T(S::hd_pass_failed))
				: said(state.neural_error, S::hd_pass_failed));
	} else {
		add(S::hn_neural, CheckState::Warn, T(S::hd_neural_idle));
	}

	if (settings.debug_view != 0 || settings.neural_debug_view != 0)
		add(S::hn_inspection, CheckState::Warn, T(S::hd_inspection),
			T(S::ha_inspection));

	const uint32_t errors = diag_count(DiagLevel::Error);
	const uint32_t warnings = diag_count(DiagLevel::Warn);
	if (errors != 0)
		add(S::hn_log, CheckState::Warn,
			format(T(errors == 1 ? (warnings == 1 ? S::hd_log_error_warning : S::hd_log_error_warnings)
				: (warnings == 1 ? S::hd_log_errors_warning : S::hd_log_errors_warnings)), errors, warnings));
	else if (warnings != 0)
		add(S::hn_log, CheckState::Warn, format(T(warnings == 1 ? S::hd_log_warning : S::hd_log_warnings), warnings));
	else
		add(S::hn_log, CheckState::Ok, T(S::hd_log_clean));

	return out;
}

std::wstring diag_report_text(const PanelState &state, const Settings &settings)
{
	diag_collect_environment_gpu();
	const DiagEnvironment &e = diag_environment();
	std::wstring out;
	out.reserve(16 * 1024);

	SYSTEMTIME now{};
	GetLocalTime(&now);

	out += L"Aeon SR diagnostic report\n";
	out += L"=========================\n";
	out += wformat(L"generated   %04u-%02u-%02u %02u:%02u:%02u\n",
		now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	out += wformat(L"add-on      Aeon SR %s (built %s)\n", e.addon_version.c_str(), e.build_stamp.c_str());
	out += wformat(L"installed   %s\n", e.addon_path.c_str());
	out += wformat(L"game        %s\n", e.exe_path.c_str());
	out += wformat(L"windows     %s\n", e.os_version.c_str());
	out += e.reshade_path.empty()
		? std::wstring(L"reshade     not detected\n")
		: wformat(L"reshade     %s  %s\n",
			e.reshade_version.empty() ? L"unknown version" : e.reshade_version.c_str(),
			e.reshade_path.c_str());
	out += wformat(L"api         %hs\n", state.api_name != nullptr ? state.api_name : "unknown");
	out += wformat(L"log         %s\n", diag_log_path().c_str());
	out += L"\n";

	const std::vector<DiagCheck> checks = diag_checks(state, settings);
	out += L"CHECKS\n";
	for (const DiagCheck &c : checks) {
		out += wformat(L"  [%hs] %-18hs %hs\n", check_state_label(c.state), c.name, c.detail.c_str());
		if (!c.tech.empty())
			out += wformat(L"         %-18hs %hs\n", "", c.tech.c_str());
		if (!c.action.empty())
			out += wformat(L"                            -> %hs\n", c.action.c_str());
	}
	out += L"\n";

	out += L"GPU\n";
	for (size_t i = 0; i < e.adapters.size(); ++i) {
		const DiagAdapter &a = e.adapters[i];
		out += wformat(L"  adapter %zu%s  %s\n", i, a.active ? L" (primary)" : L"", a.name.c_str());
		out += wformat(L"    vendor 0x%04X  device 0x%04X  subsys 0x%08X  rev 0x%02X\n",
			a.vendor_id, a.device_id, a.subsys_id, a.revision);
		out += wformat(L"    video memory %llu MB, shared %llu MB, driver %s\n",
			static_cast<unsigned long long>(a.dedicated_vram / (1024ull * 1024ull)),
			static_cast<unsigned long long>(a.shared_memory / (1024ull * 1024ull)),
			a.driver_version.empty() ? L"unknown" : a.driver_version.c_str());
	}
	if (e.nv_driver_major != 0)
		out += wformat(L"  NVIDIA driver %u.%02u\n", e.nv_driver_major, e.nv_driver_minor);
	if (e.nv_architecture != 0)
		out += wformat(L"  architecture 0x%03X (%hs), compute capability %u.%u\n",
			e.nv_architecture, neural_arch_name(e.nv_architecture),
			neural_capability_for_architecture(e.nv_architecture) / 10u,
			neural_capability_for_architecture(e.nv_architecture) % 10u);
	out += L"\n";

	out += wformat(L"FILES  (in %s unless a path is given)\n", e.addon_dir.c_str());
	for (const DiagFile &f : e.files) {
		if (!f.present) {
			out += wformat(L"  [missing] %s\n", f.label);
			continue;
		}
		const bool elsewhere = e.addon_dir.empty() ||
			f.path.compare(0, e.addon_dir.size(), e.addon_dir) != 0;
		out += wformat(L"  [found  ] %-36s %7llu KB  %-14s %s%s%s\n", f.label,
			static_cast<unsigned long long>(f.size / 1024ull),
			f.version.empty() ? L"-" : f.version.c_str(),
			f.modified.empty() ? L"-" : f.modified.c_str(),
			elsewhere ? L"  " : L"", elsewhere ? f.path.c_str() : L"");
	}
	if (!e.foreign_modules.empty())
		out += wformat(L"  upscaling modules in this process (Aeon SR's own included): %s\n", e.foreign_modules.c_str());
	out += L"\n";

	out += L"INPUTS\n";
	out += wformat(L"  colour  %ux%u fmt %u\n",
		state.color_info.width, state.color_info.height, state.color_info.format);
	const std::wstring flow_note = state.flow_note.empty() ? std::wstring() : L" - " + state.flow_note;
	out += wformat(L"  motion  %ux%u fmt %u  [%hs]%s\n",
		state.motion_info.width, state.motion_info.height, state.motion_info.format,
		motion_provider_label(state.motion_provider), flow_note.c_str());
	out += wformat(L"  depth   %ux%u fmt %u  [%hs]%s%s%s%s%hs%hs\n",
		state.depth_info.width, state.depth_info.height, state.depth_info.format,
		depth_provider_label(state.depth_provider),
		state.depth_reversed ? L", reversed" : L"",
		state.depth_upside_down ? L", upside down" : L"",
		state.depth_mirrored ? L", mirrored" : L"",
		state.depth_logarithmic ? L", logarithmic" : L"",
		state.depth_overridden.empty() ? "" : "; different in ", state.depth_overridden.c_str());
	if (state.probe_valid)
		out += wformat(L"  motion probe  %.0f%% non-zero, mean %.2f px, max %.2f px\n",
			static_cast<double>(state.probe_nonzero_pct),
			static_cast<double>(state.probe_mean_px), static_cast<double>(state.probe_max_px));
	out += wformat(L"  camera motion %.2f px, jitter %hs, frame %.2f ms presentation to presentation\n",
		static_cast<double>(state.last_motion_px),
		state.jitter_refused ? "refused"
			: state.jitter_in_frame && state.jitter_still ? "armed, nothing drawn took it, the frame left still"
			: state.jitter_in_frame && state.jitter_mixed ? "drawn into the game, mixed this frame"
			: state.jitter_in_frame ? "drawn into the game"
			: state.jitter_withheld && state.jitter_active
			? "not drawn, the game jitters its own frame; resampled as it is scaled down"
			: state.jitter_withheld ? "not drawn, the game jitters its own frame"
			: state.jitter_active ? "resampled" : "idle",
		static_cast<double>(state.frame_time_ms));
	if (state.jitter_in_frame || state.jitter_held_lists != 0 || state.jitter_note[0] != '\0')
		out += wformat(L"  jitter draws: %u moved, %u on the game's grid, %u replayed at an older "
			L"offset, %u not where the upscaler was told, %u with another depth buffer of the "
			L"scene's size; %u lists no longer moved%hs%hs\n",
			state.jitter_moved_draws, state.jitter_plain_draws, state.jitter_replayed_draws,
			state.jitter_elsewhere_draws, state.jitter_aliased_draws, state.jitter_held_lists,
			state.jitter_note[0] != '\0' ? " - " : "", state.jitter_note);
	out += L"\n";

	out += L"UPSCALER\n";
	out += wformat(L"  active      %hs  (%hs)\n",
		!state.upscaler_label.empty() ? state.upscaler_label.c_str()
			: (state.active_name != nullptr ? state.active_name : "none"),
		status_label(state.active_status));
	if (!state.active_error.empty())
		out += wformat(L"  detail      %s\n", state.active_error.c_str());
	if (state.active_is_fsr || state.active_is_xess || state.active_is_vsr)
		out += wformat(L"  resolution  %ux%u -> %ux%u  dll %hs  init %hs\n",
			state.upscaler_render_width, state.upscaler_render_height,
			state.upscaler_out_width, state.upscaler_out_height,
			bool_label(state.upscaler_dll_present), bool_label(state.upscaler_initialized));
	else
		out += wformat(L"  resolution  %ux%u -> %ux%u  dll %hs  init %hs  evals %llu\n",
			state.ngx_render_width, state.ngx_render_height, state.ngx_width, state.ngx_height,
			bool_label(state.ngx_dll_present), bool_label(state.ngx_initialized),
			static_cast<unsigned long long>(state.ngx_eval_count));
	if (state.active_is_fsr) {
		out += wformat(L"  provider    %s\n", state.fsr_provider_version.empty()
			? L"(the runtime did not say)" : state.fsr_provider_version.c_str());
		if (!state.fsr_providers.empty()) {
			out += L"  offered     ";
			for (const std::string &p : state.fsr_providers)
				out += wformat(L"%hs  ", p.c_str());
			out += L"\n";
		}
	}
	if (state.active_is_vsr)
		out += wformat(L"  RTX VSR     capable %hs  enhancement %hs  in use %hs  level %u\n",
			bool_label(state.vsr_capable), state.vsr_enabled ? "on" : "off",
			bool_label(state.vsr_in_use), state.vsr_level);
	if (state.bridge_kind != BridgeKind::Native12)
		out += wformat(L"  bridge      %hs  %hs%s\n",
			state.bridge_name != nullptr ? state.bridge_name : "none",
			state.bridge_sync != nullptr ? state.bridge_sync : "",
			state.bridge_fsr_adapter_matched ? L"" : L"  (adapter not matched yet)");
	if (!state.bridge_error.empty())
		out += wformat(L"  bridge err  %s\n", state.bridge_error.c_str());
	out += L"\n";

	out += L"NEURAL RENDERING\n";
	out += wformat(L"  runtime     %hs   shim %hs   initialised %hs   evals %llu\n",
		bool_label(state.neural_dll_present), bool_label(state.neural_shim_present),
		bool_label(state.neural_initialized),
		static_cast<unsigned long long>(state.neural_eval_count));
	out += wformat(L"  status      %hs%s\n", status_label(state.neural_status),
		state.neural_crashed ? L"  (faulted)" : L"");
	if (!state.neural_error.empty())
		out += wformat(L"  detail      %s\n", state.neural_error.c_str());
	const std::wstring min_arch = state.neural_requirement_known && state.neural_min_architecture != 0
		? wformat(L"0x%03X (%hs)", state.neural_min_architecture,
			neural_arch_name(state.neural_min_architecture))
		: std::wstring(L"not answered");
	out += wformat(L"  gpu         0x%03X (%hs)   runtime minimum %s\n",
		state.neural_gpu_architecture, neural_arch_name(state.neural_gpu_architecture),
		min_arch.c_str());
	if (!state.neural_runtime_file.empty()) {
		out += wformat(L"  file        %s\n", state.neural_runtime_file.c_str());
		out += wformat(L"  version     %hs   certificate table %hs\n",
			state.neural_runtime_version.empty() ? "(none)" : state.neural_runtime_version.c_str(),
			state.neural_runtime_certificate ? "present" : "none");
		out += wformat(L"  sha256      %hs\n",
			state.neural_runtime_sha256.empty() ? "(not read)" : state.neural_runtime_sha256.c_str());
		out += wformat(L"  built for   %hs\n",
			state.neural_runtime_cards.empty() ? "(names no card)" : state.neural_runtime_cards.c_str());
	}
	out += wformat(L"  kernels     %hs\n",
		state.neural_runtime_kernels.empty() ? "none named in the file"
			: state.neural_runtime_kernels.c_str());
	if (state.neural_runtime_minimum != 0u)
		out += wformat(L"  card check  runtime accepts 0x%03X and newer; %s\n",
			state.neural_runtime_minimum,
			state.neural_reported_architecture != 0u
				? wformat(L"told 0x%03X in place of this card's own", state.neural_reported_architecture).c_str()
				: L"told the truth");
	if (state.neural_runtime_candidates.size() > 1u) {
		out += L"  candidates ";
		for (const std::wstring &c : state.neural_runtime_candidates)
			out += L" " + c;
		out += L"\n";
	}
	if (state.neural_required_driver_major != 0)
		out += wformat(L"  driver      %u.%02u   runtime requires %u.%02u\n",
			state.neural_driver_major, state.neural_driver_minor,
			state.neural_required_driver_major, state.neural_required_driver_minor);
	else
		out += wformat(L"  driver      %u.%02u   runtime minimum not read\n",
			state.neural_driver_major, state.neural_driver_minor);
	out += wformat(L"  chain       %u of %u passes, model %ux%u, %.0f%% of it per frame, %.2f ms\n",
		state.neural_passes_built, clamp_neural_passes(settings.neural_passes),
		state.neural_model_width, state.neural_model_height,
		state.neural_model_height != 0 ? 100.0 * state.neural_eval_rows / state.neural_model_height : 0.0,
		static_cast<double>(state.neural_gpu_ms));
	out += L"\n";

	append_settings(out, settings);
	out += L"\n";

	out += L"LOG\n";
	const std::vector<DiagEntry> recent = diag_recent(kDiagHistory, DiagLevel::Info);
	if (recent.empty()) {
		out += L"  (empty)\n";
	} else {
		for (const DiagEntry &r : recent)
			out += wformat(L"  [%8.3f] %-5hs %-9hs %s\n", static_cast<double>(r.time_s),
				diag_level_label(r.level), r.tag.c_str(), r.text.c_str());
	}
	return out;
}

std::wstring diag_write_report(const PanelState &state, const Settings &settings,
	std::wstring *error)
{
	if (error != nullptr)
		error->clear();
	const DiagEnvironment &env = diag_environment();
	const std::wstring dir = env.addon_dir.empty() ? exe_directory_w() : env.addon_dir;
	const std::wstring path = join_path(dir, L"AeonSR_report.txt");
	const std::wstring text = diag_report_text(state, settings);

	FILE *f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") != 0 || f == nullptr) {
		if (error != nullptr)
			*error = L"could not write " + path + L" - the folder may be read-only";
		diag_error("report", *error);
		return {};
	}
	fwprintf(f, L"%s", text.c_str());
	fclose(f);
	diag_logf(DiagLevel::Info, "report", L"wrote %s", path.c_str());
	return path;
}

namespace {

bool technical(const std::string &s)
{
	if (s.find("(0x") != std::string::npos)
		return true;
	for (size_t at = s.find(" failed"); at != std::string::npos; at = s.find(" failed", at + 1)) {
		size_t b = at;
		while (b > 0 && (std::isalnum(static_cast<unsigned char>(s[b - 1])) != 0 || s[b - 1] == '_'))
			--b;
		for (size_t i = b + 1; i < at; ++i)
			if (std::islower(static_cast<unsigned char>(s[i - 1])) != 0 &&
				std::isupper(static_cast<unsigned char>(s[i])) != 0)
				return true;
	}
	return false;
}

}

bool diag_is_technical(const std::string &s)
{
	if (technical(s) || s.find("::") != std::string::npos)
		return true;
	static const char *const kNames[] = { "OptiScaler", "ReShade", "XeSS", "AeonSR", "AeonSRHost", "GeForce", "DirectX",
		"FidelityFX", "OpenGL", "NVIDIA", "Direct3D", "HDR10", "scRGB", "sRGB", "DLSS", "RTX", "VSR", "GPU", "API", "DLL" };
	size_t i = 0;
	while (i < s.size()) {
		while (i < s.size() && !std::isalnum(static_cast<unsigned char>(s[i])))
			++i;
		size_t j = i;
		while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_'))
			++j;
		const std::string w = s.substr(i, j - i);
		i = j;
		if (w.empty())
			continue;
		bool named = false;
		for (const char *n : kNames)
			named = named || w.rfind(n, 0) == 0;
		if (named)
			continue;
		if (w.find('_') != std::string::npos || w == "d3d12" || w == "D3D12" || w == "d3d11" || w == "D3D11" ||
			w == "hook" || w == "hooked")
			return true;
		for (size_t k = 1; k < w.size(); ++k)
			if (std::islower(static_cast<unsigned char>(w[k - 1])) && std::isupper(static_cast<unsigned char>(w[k])))
				return true;
	}
	return false;
}

std::string diag_said(const std::string &english, i18n::Lang lang, i18n::S fallback, std::string *tech)
{
	tech->clear();
	if (english.empty())
		return std::string();
	std::string said, rest;
	if (i18n::lookup(lang, english, &said, &rest)) {
		if (rest.empty())
			return said;
		if (lang == i18n::Lang::en_US && !diag_is_technical(rest))
			return english;
		*tech = english;
		while (!said.empty() && (said.back() == ' ' || said.back() == ':'))
			said.pop_back();
		const size_t wide_colon = said.size() >= 3 ? said.size() - 3 : std::string::npos;
		if (wide_colon != std::string::npos && said.compare(wide_colon, 3, "\xEF\xBC\x9A") == 0)
			said.erase(wide_colon);
		return said;
	}
	if (lang == i18n::Lang::en_US && !diag_is_technical(english))
		return english;
	*tech = english;
	return fallback == i18n::S::Count ? std::string() : std::string(i18n::text(lang, fallback));
}

std::string diag_cards_text(const std::string &english_cards, i18n::Lang lang)
{
	if (english_cards.rfind("no RTX", 0) == 0)
		return i18n::text(lang, i18n::S::hd_cards_none);
	std::string out;
	std::string num;
	const auto flush = [&]() {
		if (num.empty())
			return;
		out += out.empty() ? "RTX " : std::string(i18n::text(lang, i18n::S::list_sep));
		out += num;
		num.clear();
	};
	for (char c : english_cards) {
		if (std::isdigit(static_cast<unsigned char>(c)))
			num.push_back(c);
		else
			flush();
	}
	flush();
	return out.empty() ? english_cards : out;
}

const char *diag_arch_text(uint32_t architecture, i18n::Lang lang)
{
	const char *name = neural_arch_name(architecture);
	return std::strncmp(name, "an architecture", 15) == 0 ? i18n::text(lang, i18n::S::hd_unknown_arch) : name;
}

std::string diag_player_text(const std::string &raw, std::string *tech)
{
	std::string s = raw;
	for (const char *prefix : { "NVSDK_NGX_D3D12_", "NVSDK_NGX_D3D11_", "NVSDK_NGX_VULKAN_", "NVSDK_NGX_" }) {
		const size_t len = std::char_traits<char>::length(prefix);
		for (size_t at = s.find(prefix); at != std::string::npos; at = s.find(prefix, at))
			s.erase(at, len);
	}
	tech->clear();
	const size_t dash = s.rfind(" - ");
	if (dash != std::string::npos && technical(s.substr(dash + 3))) {
		*tech = s.substr(dash + 3);
		return s.substr(0, dash);
	}
	if (!technical(s))
		return s;
	*tech = s;
	for (size_t at = s.find(" (0x"); at != std::string::npos; at = s.find(" (0x", at)) {
		const size_t close = s.find(')', at);
		s.erase(at, close == std::string::npos ? std::string::npos : close - at + 1);
	}
	static const struct { const char *call, *words; } kCalls[] = {
		{ "D3D12 CreateFeature failed", "start failed" },
		{ "CreateFeature failed", "start failed" },
	};
	for (const auto &c : kCalls)
		for (size_t at = s.find(c.call); at != std::string::npos; at = s.find(c.call, at))
			s.replace(at, std::char_traits<char>::length(c.call), c.words);
	if (!s.empty())
		s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
	return s;
}

}

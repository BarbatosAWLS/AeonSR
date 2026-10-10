#include "aeon_sr/core/diag_report.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/gpu_vendor.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/ngx/ngx_dlssnr.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>

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
	char buf[512]{};
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return buf;
}

const char *bool_label(bool v) noexcept { return v ? "yes" : "no"; }

void append_settings(std::wstring &out, const Settings &s)
{
	out += L"SETTINGS\n";
	const std::wstring backend = s.backend < kBackendChoiceCount
		? wformat(L"%u", s.backend)
		: std::wstring(L"unset (settled on the first frame)");
	out += wformat(L"  enabled=%hs  backend=%s  mode=%u  preset=%u  sharpness=%.2f\n",
		bool_label(s.enabled), backend.c_str(), s.upscale_mode, s.render_preset,
		static_cast<double>(s.sharpness));
	out += wformat(L"  upscale_effects=%hs  jitter=%hs  flow_quality=%u  mv_probe=%hs\n",
		bool_label(s.upscale_effects), bool_label(s.spatial_jitter),
		s.internal_flow_quality, bool_label(s.mv_probe));
	out += wformat(L"  vsr_input_format=%u  fsr_provider=%hs\n", s.vsr_input_format,
		s.fsr_provider.empty() ? "auto" : s.fsr_provider.c_str());
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
	out += wformat(L"  debug_view=%u  neural_debug_view=%u  osd=%hs  verbose_log=%hs\n",
		s.debug_view, s.neural_debug_view, bool_label(s.show_osd), bool_label(s.debug_info));
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

CheckState diag_worst(const std::vector<DiagCheck> &checks) noexcept
{
	CheckState worst = CheckState::Off;
	for (const DiagCheck &c : checks)
		if (static_cast<unsigned>(c.state) > static_cast<unsigned>(worst))
			worst = c.state;
	return worst;
}

std::vector<DiagCheck> diag_checks(const PanelState &state, const Settings &settings)
{
	const DiagEnvironment &e = diag_environment();
	std::vector<DiagCheck> out;
	auto add = [&out](const char *name, CheckState s, std::string detail, std::string action = {}) {
		DiagCheck c;
		c.name = name;
		c.state = s;
		c.detail = std::move(detail);
		c.action = std::move(action);
		out.push_back(std::move(c));
	};

	add("Graphics API", CheckState::Ok, state.api_name != nullptr ? state.api_name : "unknown");
	if (state.bridge_name != nullptr)
		add("Bridge to Direct3D 12", state.bridge_any_ready ? CheckState::Ok : CheckState::Warn,
			state.bridge_name,
			state.bridge_any_ready ? "" : "The add-on could not reach a Direct3D 12 device yet.");
	else if (!state.bridge_error.empty())
		add("Bridge to Direct3D 12", CheckState::Fail, "none",
			"Every upscaler here is Direct3D 12 work and this game's API could not be bridged.");

	if (state.upscalers_remote && state.host_ready)
		add("Where the upscalers run", CheckState::Ok,
			format("AeonSRHost.exe, process %u", state.host_pid),
			"This game is a 32-bit program and the upscalers are 64-bit, so they run beside it.");
	else if (state.upscalers_remote)
		add("Where the upscalers run", CheckState::Off,
			"AeonSRHost.exe, not started yet",
			"It starts with the first upscaled frame. Until then nothing here knows which "
			"upscalers you have, so nothing is claimed about them.");

	if (state.gpu_name.empty()) {
		add("GPU", CheckState::Warn, "not identified yet",
			"Open the overlay once a frame has rendered.");
	} else {
		std::string gpu = diag_narrow(state.gpu_name);
		if (e.nv_driver_major != 0)
			gpu += format(", driver %u.%02u", e.nv_driver_major, e.nv_driver_minor);
		add("GPU", CheckState::Ok, std::move(gpu));
	}

	if (!e.reshade_version.empty())
		add("ReShade", CheckState::Ok, diag_narrow(e.reshade_version));

	if (!state.motion_wanted) {
		add("Motion vectors", CheckState::Off,
			"not needed - nothing is running that consumes motion");
	} else if (state.motion_provider == MotionProvider::None && state.flow_note_benign) {
		add("Motion vectors", CheckState::Warn, diag_narrow(state.flow_note),
			"Normal for the first frames after a resolution change.");
	} else if (state.motion_provider == MotionProvider::None) {
		add("Motion vectors", CheckState::Fail,
			state.flow_note.empty() ? "none this frame" : diag_narrow(state.flow_note),
			"Every upscaler here needs motion. Without it they stay idle.");
	} else if (state.probe_valid && state.probe_nonzero_pct <= 0.0f && state.probe_moving) {
		add("Motion vectors", CheckState::Fail, "every sampled vector was zero while the camera moved",
			"The estimator is running but writing nothing.");
	} else if (state.probe_valid) {
		add("Motion vectors", CheckState::Ok,
			format("%s, %.0f%% non-zero, mean %.2f px",
				motion_provider_label(state.motion_provider),
				static_cast<double>(state.probe_nonzero_pct),
				static_cast<double>(state.probe_mean_px)));
	} else {
		add("Motion vectors", CheckState::Ok, motion_provider_label(state.motion_provider));
	}

	if (state.depth_provider == DepthProvider::None) {
		add("Depth buffer", CheckState::Warn, "none found",
			"Pick the game's depth buffer in ReShade's own depth settings. "
			"The add-on works without it, with slightly worse edges.");
	} else {
		const std::string depth_line = format("%s, %s%s%s, far %.0f",
			depth_provider_label(state.depth_provider), state.depth_reversed ? "reversed" : "normal",
			state.depth_upside_down ? ", upside down" : "", state.depth_mirrored ? ", mirrored" : "",
			static_cast<double>(state.depth_far_plane));
		if (state.depth_overridden.empty())
			add("Depth buffer", CheckState::Ok, depth_line);
		else
			add("Depth buffer", CheckState::Warn, depth_line,
				"These effects carry their own depth definitions in the preset and see a different depth: " +
				state.depth_overridden + ". Remove them from the preset file.");
	}

	const bool no_upscaler = !settings.enabled ||
		settings.backend == static_cast<unsigned>(BackendChoice::None);
	if (no_upscaler) {
		add("Upscaler", CheckState::Off, "turned off - the frame is left as the game rendered it");
	} else {
		const std::string name = !state.upscaler_label.empty() ? state.upscaler_label
			: std::string(state.active_name != nullptr ? state.active_name : "unknown");
		std::string detail = format("%s: %s", name.c_str(), status_label(state.active_status));
		std::string action;
		CheckState s = CheckState::Ok;
		switch (state.active_status) {
		case UpscalerStatus::Ready:
			detail += format(", %ux%u -> %ux%u",
				state.active_is_fsr || state.active_is_xess || state.active_is_vsr
					? state.upscaler_render_width : state.ngx_render_width,
				state.active_is_fsr || state.active_is_xess || state.active_is_vsr
					? state.upscaler_render_height : state.ngx_render_height,
				state.active_is_fsr || state.active_is_xess || state.active_is_vsr
					? state.upscaler_out_width : state.ngx_width,
				state.active_is_fsr || state.active_is_xess || state.active_is_vsr
					? state.upscaler_out_height : state.ngx_height);
			break;
		case UpscalerStatus::MissingRuntime:
			s = CheckState::Fail;
			action = std::string("Its runtime DLL is not beside ") + kAddonFileName +
				". Reinstall the release folder whole.";
			break;
		case UpscalerStatus::UnsupportedApi:
			s = CheckState::Fail;
			action = "This upscaler does not run on this graphics API. Pick another one.";
			break;
		case UpscalerStatus::UnsupportedGpu:
			s = CheckState::Fail;
			action = "This upscaler needs a different GPU vendor. Pick another one.";
			break;
		case UpscalerStatus::NeedMotionVectors:
			s = CheckState::Warn;
			action = "Waiting for the motion estimator; it needs two frames after a resolution change.";
			break;
		case UpscalerStatus::NeedDepth:
			s = CheckState::Warn;
			action = "Only DLSS needs depth. Menus have none, so this clears itself once the "
				"game draws a scene. If it never clears, pick FSR or XeSS.";
			break;
		case UpscalerStatus::Crashed:
			s = CheckState::Fail;
			action = "The runtime faulted and was switched off for this session. Restart the game.";
			break;
		case UpscalerStatus::InitFailed:
		case UpscalerStatus::EvaluateFailed:
			s = CheckState::Fail;
			break;
		case UpscalerStatus::Loading:
			s = CheckState::Warn;
			action = "Still coming up on a thread of its own. The frame goes through "
				"unupscaled until it lands, which takes a few seconds the first time.";
			break;
		case UpscalerStatus::Idle:
			s = CheckState::Warn;
			break;
		}
		if (!state.active_error.empty())
			detail += " - " + diag_narrow(state.active_error);
		add("Upscaler", s, std::move(detail), std::move(action));
	}

	if (!state.ngx_layer.empty() && state.ngx_layer_nvidia) {
		if (state.ngx_layer_relevant && !state.host_error.empty())
			add("Other DLSS program", CheckState::Fail, "loaded in the game: " + diag_narrow(state.ngx_layer),
				"Aeon SR's DLSS and DLSS neural rendering run in AeonSRHost.exe, apart from it, and it did not "
				"start: " + diag_narrow(state.host_error));
		else
			add("Other DLSS program", CheckState::Ok, "loaded in the game: " + diag_narrow(state.ngx_layer),
				state.ngx_layer_relevant
					? "Aeon SR's DLSS and DLSS neural rendering run in AeonSRHost.exe, apart from it."
					: "Aeon SR's DLSS and DLSS neural rendering would run in AeonSRHost.exe, apart from it.");
	}

	if (state.native_dlss)
		add("Game's own DLSS", CheckState::Warn,
			"loaded by the game: " + diag_narrow(state.native_dlss_modules),
			"Use the game's own setting and set the upscaler here to None. Two in series ghost.");

	if (!settings.neural_render) {
		add("Neural rendering", CheckState::Off, "turned off");
	} else if (state.neural_last == 1) {
		std::string detail = format("running, %u pass%s at %ux%u",
			state.neural_passes_built, state.neural_passes_built == 1 ? "" : "es",
			state.neural_model_width, state.neural_model_height);
		if (state.neural_eval_rows != 0 && state.neural_eval_rows < state.neural_model_height)
			detail += format(", up to %.0f%% of the model per frame",
				100.0 * state.neural_eval_rows / state.neural_model_height);
		if (state.neural_gpu_ms > 0.0f)
			detail += format(", %.2f ms", static_cast<double>(state.neural_gpu_ms));
		if (!state.neural_runtime_cards.empty())
			detail += ", runtime built for " + state.neural_runtime_cards;
		if (state.neural_reported_architecture != 0u)
			detail += " (its own card check bypassed)";
		add("Neural rendering", CheckState::Ok,
			state.upscalers_remote ? detail + ", in the helper process" : detail);
	} else if (!vendor_may_be_nvidia(state.gpu_vendor)) {
		add("Neural rendering", CheckState::Fail,
			format("needs an NVIDIA GPU (%s detected)", vendor_label(state.gpu_vendor)),
			"Everything else in Aeon SR works on this card.");
	} else if (!state.neural_dll_present) {
		add("Neural rendering", CheckState::Fail, "nvngx_dlssnr.dll not found",
			"It is not shipped with Aeon SR and the driver does not install it. "
			"Copy it from a game that ships DLSS neural rendering into the add-on folder.");
	} else if (!state.neural_shim_present) {
		add("Neural rendering", CheckState::Fail, "ngxshim\\nvngx.dll is missing",
			std::string("Copy the ngxshim folder from the release beside ") + kAddonFileName + ".");
	} else if (!state.neural_runtime_serves_card) {
		add("Neural rendering", CheckState::Fail,
			state.neural_runtime_cards.empty()
				? std::string("this nvngx_dlssnr.dll carries no code for this GPU")
				: "this nvngx_dlssnr.dll is built for " + state.neural_runtime_cards,
			state.neural_allgpu_build_targets
				? format("This GPU is %s. A build of nvngx_dlssnr.dll for all GPUs runs here: put it in "
					"runtime\\dlss5-allgpu\\ beside the add-on, then turn neural rendering off and on. "
					"Everything else works.", neural_arch_name(state.neural_gpu_architecture))
				: format("This GPU is %s, which no nvngx_dlssnr.dll build is known to run on. "
					"Everything else in Aeon SR works on this card.",
					neural_arch_name(state.neural_gpu_architecture)));
	} else if (state.neural_adapter_unsupported) {
		add("Neural rendering", CheckState::Fail,
			format("NVIDIA's runtime needs %s or newer", neural_arch_name(state.neural_min_architecture)),
			"Everything else in Aeon SR works on this card.");
	} else if (state.neural_driver_too_old) {
		add("Neural rendering", CheckState::Fail,
			format("driver %u.%02u, the runtime needs %u.%u",
				state.neural_driver_major, state.neural_driver_minor,
				state.neural_required_driver_major, state.neural_required_driver_minor),
			"Update the NVIDIA driver.");
	} else if (state.neural_feature_unsupported) {
		add("Neural rendering", CheckState::Fail, "the runtime refused to create the pass on this GPU",
			"Not retryable. Everything else in Aeon SR works on this card.");
	} else if (state.neural_crashed) {
		add("Neural rendering", CheckState::Fail, "the runtime faulted and was switched off",
			"Restart the game. If it repeats, the log names the call that faulted.");
	} else if (state.neural_last == -1) {
		add("Neural rendering", CheckState::Fail,
			state.neural_error.empty() ? std::string("the pass failed this frame")
				: diag_narrow(state.neural_error));
	} else if (state.neural_last == 1) {
		std::string detail = format("running, %u pass%s at %ux%u",
			state.neural_passes_built, state.neural_passes_built == 1 ? "" : "es",
			state.neural_model_width, state.neural_model_height);
		if (state.neural_eval_rows != 0 && state.neural_eval_rows < state.neural_model_height)
			detail += format(", up to %.0f%% of the model per frame",
				100.0 * state.neural_eval_rows / state.neural_model_height);
		if (state.neural_gpu_ms > 0.0f)
			detail += format(", %.2f ms", static_cast<double>(state.neural_gpu_ms));
		const bool short_chain = state.neural_passes_built != 0 &&
			state.neural_passes_built < clamp_neural_passes(settings.neural_passes);
		add("Neural rendering", short_chain ? CheckState::Warn : CheckState::Ok, std::move(detail),
			short_chain ? "The runtime would not build every pass; the chain is shorter than asked for." : "");
	} else {
		add("Neural rendering", CheckState::Warn, "idle - waiting for a frame to work on");
	}

	if (settings.debug_view != 0 || settings.neural_debug_view != 0)
		add("Inspection view", CheckState::Warn, "the frame is being replaced by a debug picture",
			"Set both Debug view rows to Off to see the game again.");

	const uint32_t errors = diag_count(DiagLevel::Error);
	const uint32_t warnings = diag_count(DiagLevel::Warn);
	if (errors != 0)
		add("Log", CheckState::Warn,
			format("%u error%s and %u warning%s so far", errors, errors == 1 ? "" : "s",
				warnings, warnings == 1 ? "" : "s"),
			"Save the diagnostic report; it carries them with the machine they happened on.");
	else if (warnings != 0)
		add("Log", CheckState::Warn, format("%u warning%s so far", warnings, warnings == 1 ? "" : "s"));
	else
		add("Log", CheckState::Ok, "nothing has failed");

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
		out += wformat(L"  already loaded by this game: %s\n", e.foreign_modules.c_str());
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
		out += wformat(L"  driver      %u.%02u   runtime requires %u.%u\n",
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

}

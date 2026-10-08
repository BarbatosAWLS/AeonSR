#include "aeon_sr/core/settings.hpp"

#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

namespace aeon_sr {
namespace {

std::string narrow(const std::wstring &ws)
{
	if (ws.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), n, nullptr, nullptr);
	return out;
}

bool file_exists_path(const std::wstring &path)
{
	const DWORD attrs = GetFileAttributesW(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}

std::wstring module_directory(HMODULE module)
{
	wchar_t path[MAX_PATH]{};
	const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return L".";
	std::wstring full(path, path + n);
	const auto slash = full.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return L".";
	return full.substr(0, slash);
}

std::wstring join_path(const std::wstring &dir, const wchar_t *file)
{
	if (dir.empty())
		return file;
	if (dir.back() == L'\\' || dir.back() == L'/')
		return dir + file;
	return dir + L'\\' + file;
}

unsigned int normalize_render_preset(unsigned int preset) noexcept
{
	for (const unsigned int v : kFunctionalRenderPresets)
		if (v == preset)
			return preset;
	return 0u;
}

unsigned int clamp_neural_passes(unsigned int passes) noexcept
{
	if (passes < 1u)
		return 1u;
	return passes > kNeuralPassMax ? kNeuralPassMax : passes;
}

float neural_chain_relative_cost(float model_scale, unsigned int passes, float row_fraction) noexcept
{
	const float s = std::isfinite(model_scale) ? std::clamp(model_scale, 0.0f, 1.0f) : 1.0f;
	const float rows = std::isfinite(row_fraction) ? std::clamp(row_fraction, 0.0f, 1.0f) : 1.0f;
	const float one = kNeuralPassFixedCost + kNeuralPassPixelCost * s * s * rows;
	const float full = kNeuralPassFixedCost + kNeuralPassPixelCost;
	return static_cast<float>(clamp_neural_passes(passes)) * one / full;
}

unsigned int clamp_neural_mode(unsigned int mode) noexcept
{
	return mode < kNeuralModeCount ? mode : 0u;
}

unsigned int neural_mode_width(unsigned int frame_w, float scale, unsigned int mode) noexcept
{
	constexpr unsigned int kGrid = 256u;
	const float base = scale > 0.0f && scale < 1.0f ? scale : 1.0f;
	const unsigned int detail = neural_model_width(frame_w, base);
	unsigned int width = detail;
	mode = clamp_neural_mode(mode);
	for (unsigned int m = 1u; m <= mode; ++m) {
		unsigned int want = neural_model_width(frame_w, base * kNeuralModeKeep[m]);
		const unsigned int below = width > kGrid ? (width - 1u) / kGrid * kGrid : 0u;
		if (want >= width)
			want = below;
		if (want == 0u || static_cast<float>(want) < kNeuralModeKeepMin * static_cast<float>(detail))
			break;
		width = want;
	}
	return width;
}

double neural_warp_to_frame(double model_uv, double a) noexcept
{
	const double s = model_uv - 0.5;
	return 0.5 + a * s + 4.0 * (1.0 - a) * s * s * s;
}

double neural_warp_to_model(double frame_uv, double a) noexcept
{
	const double b = 4.0 * (1.0 - a);
	const double t = frame_uv - 0.5;
	if (b < 1e-9)
		return 0.5 + t / a;
	const double p = a / b;
	const double k = 2.0 * std::sqrt(p / 3.0);
	const double arg = (3.0 * t / (b * p)) * std::sqrt(3.0 / p) * 0.5;
	return 0.5 + k * std::sinh(std::asinh(arg) / 3.0);
}

float clamp_neural_unit(float v, float fallback, float max_value) noexcept
{
	if (!std::isfinite(v))
		return fallback;
	if (!std::isfinite(max_value) || max_value < 0.0f)
		max_value = kNeuralUnitMax;
	return std::clamp(v, 0.0f, max_value);
}

unsigned int clamp_accum_iterations(unsigned int n) noexcept
{
	if (n < kAccumMin)
		return kAccumMin;
	return n > kAccumMax ? kAccumMax : n;
}

float clamp_neural_skin(float v, float max_value) noexcept
{
	if (!std::isfinite(v))
		return kNeuralSkinDefault;
	if (!std::isfinite(max_value) || max_value < kNeuralSkinMin)
		max_value = kNeuralSkinMax;
	return std::clamp(v, kNeuralSkinMin, max_value);
}

float clamp_neural_range(float v, float lo, float hi, float fallback) noexcept
{
	if (!std::isfinite(v))
		return fallback;
	return v < lo ? lo : (v > hi ? hi : v);
}

unsigned int neural_model_width(unsigned int frame_w, float scale) noexcept
{
	if (!(scale > 0.0f) || !(scale < 1.0f) || frame_w == 0u)
		return frame_w;
	constexpr unsigned int kGrid = 256u;
	const long r = std::lround(static_cast<double>(frame_w) * static_cast<double>(scale));
	unsigned int mw = r > 0 ? static_cast<unsigned int>(r) : 0u;
	mw &= ~1u;
	if (mw < 64u)
		mw = 64u;
	const unsigned int up = (mw + kGrid - 1u) / kGrid * kGrid;
	if (up <= frame_w)
		return up;
	const unsigned int down = (frame_w / kGrid) * kGrid;
	return down >= kGrid ? down : mw;
}

unsigned int neural_model_scale_percent(unsigned int index) noexcept
{
	const float f = index < kNeuralModelScaleCount ? kNeuralModelScales[index] : 1.0f;
	return static_cast<unsigned int>(f * 100.0f + 0.5f);
}

unsigned int neural_model_scale_index_for_percent(unsigned int percent) noexcept
{
	unsigned int best = kNeuralModelScaleDefault;
	unsigned int best_d = 1000u;
	for (unsigned int i = 0; i < kNeuralModelScaleCount; ++i) {
		const unsigned int p = neural_model_scale_percent(i);
		const unsigned int d = p > percent ? p - percent : percent - p;
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	return best;
}

float neural_model_scale_factor(unsigned int index) noexcept
{
	return index < kNeuralModelScaleCount ? kNeuralModelScales[index] : 1.0f;
}

float neural_blend_alpha(float blend) noexcept
{
	const float b = clamp_neural_unit(blend, kNeuralBlendDefault);
	return 1.0f - 0.95f * b;
}

float neural_steady_weight(float band, float blend) noexcept
{
	const float b = clamp_neural_range(band, kNeuralSteadyMin, kNeuralSteadyMax,
		kNeuralSteadyDefault);
	const float full = 0.5f / b;
	const float amount = clamp_neural_unit(blend, kNeuralBlendDefault);
	return 1.0f - (1.0f - full) * amount;
}

float neural_reject_threshold(float reject) noexcept
{
	const float r = clamp_neural_unit(reject, kNeuralRejectDefault);
	return 1.5f - 1.4f * r;
}

float neural_temporal_alpha_legacy(unsigned int level) noexcept
{
	static const float kAlpha[4] = { 1.0f, 0.5f, 0.3f, 0.15f };
	return level < 4u ? kAlpha[level] : 1.0f;
}

float neural_scale_compensation(unsigned int index) noexcept
{

	static const float kComp[kNeuralModelScaleCount] = {
		1.0f, 0.99f, 0.98f, 0.90f, 0.82f, 0.76f, 0.82f };
	return index < kNeuralModelScaleCount ? kComp[index] : 1.0f;
}

unsigned int clamp_neural_style(unsigned int style) noexcept
{
	return style < kNeuralStyleCount ? style : 0u;
}

float vsr_scale_for_mode(UpscaleMode mode) noexcept
{
	switch (mode) {
	case UpscaleMode::UltraQuality: return 1.0f / 1.3f;
	case UpscaleMode::Quality: return 1.0f / 1.5f;
	case UpscaleMode::Balanced: return 1.0f / 1.7f;
	case UpscaleMode::Performance: return 0.5f;
	case UpscaleMode::UltraPerformance: return 1.0f / 3.0f;
	case UpscaleMode::Dlaa:
	default: return 1.0f;
	}
}

float clamp_spatial_scale(float scale) noexcept
{
	if (!(scale > 0.0f) || !std::isfinite(scale))
		return kSpatialScaleDefault;
	return std::clamp(scale, kSpatialScaleMin, kSpatialScaleMax);
}

void scaled_render_size(uint32_t display_w, uint32_t display_h, float scale,
	uint32_t *out_w, uint32_t *out_h) noexcept
{
	const float s = clamp_spatial_scale(scale);

	auto even_dim = [](uint32_t display, float sc) -> uint32_t {
		if (sc >= 1.0f)
			return display;
		uint32_t v = static_cast<uint32_t>(std::lround(static_cast<double>(display) * static_cast<double>(sc)));
		if (v < 2u)
			v = 2u;
		v &= ~1u;
		if (v >= display)
			v = (display > 2u) ? (display - 2u) : 2u;
		v &= ~1u;
		if (v < 2u)
			v = 2u;
		return v;
	};
	if (out_w)
		*out_w = even_dim(display_w, s);
	if (out_h)
		*out_h = even_dim(display_h, s);
}

void load_settings(Settings &out, const std::wstring &ini_path)
{
	out = Settings{};
	const std::string path = narrow(ini_path);
	std::ifstream in(path);
	if (!in)
		return;

	auto parse_bool = [](const std::string &v) {
		return v == "1" || v == "true";
	};

	unsigned long legacy_bands = 0ul;
	bool have_mode = false;
	bool legacy_balanced = false;

	std::string line;
	while (std::getline(in, line)) {
		const auto eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = line.substr(0, eq);
		const std::string val = line.substr(eq + 1);

		if (key == "Enabled") out.enabled = parse_bool(val);
		else if (key == "ShowOsd") out.show_osd = parse_bool(val);
		else if (key == "UpscaleEffects") out.upscale_effects = parse_bool(val);
		else if (key == "RenderPreset") out.render_preset = normalize_render_preset(static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10)));
		else if (key == "Sharpness") out.sharpness = static_cast<float>(std::atof(val.c_str()));
		else if (key == "MvProbe") out.mv_probe = parse_bool(val);
		else if (key == "GpuTimings") out.gpu_timings = parse_bool(val);
		else if (key == "SpatialJitter") out.spatial_jitter = parse_bool(val);
		else if (key == "UpscaleMode") out.upscale_mode = static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10));
		else if (key == "Upscaler") {
			const unsigned int v = static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10));
			out.backend = (v < kBackendChoiceCount) ? v : Settings::kBackendUnset;
		}
		else if (key == "Backend") {
			const unsigned int v = static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10));
			out.backend = (v >= 1u && v <= kBackendChoiceCount) ? v - 1u : Settings::kBackendUnset;
		}
		else if (key == "FsrProvider") out.fsr_provider = val;
		else if (key == "VsrInputFormat") {
			const unsigned int v = static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10));
			out.vsr_input_format = (v < kVsrInputFormatCount) ? v : 0u;
		}
		else if (key == "NeuralRender") out.neural_render = parse_bool(val);
		else if (key == "NeuralStyle") out.neural_style = clamp_neural_style(static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10)));
		else if (key == "NeuralIntensity") out.neural_intensity = clamp_neural_unit(static_cast<float>(std::atof(val.c_str())), kNeuralIntensityDefault);
		else if (key == "NeuralLocalStructure") out.neural_local_structure = clamp_neural_unit(static_cast<float>(std::atof(val.c_str())), kNeuralStructureDefault, kNeuralExtendedMax);
		else if (key == "NeuralLocalTone") out.neural_local_tone = clamp_neural_unit(static_cast<float>(std::atof(val.c_str())), kNeuralToneDefault, kNeuralExtendedMax);
		else if (key == "NeuralSkinStructure") out.neural_skin_structure = clamp_neural_skin(static_cast<float>(std::atof(val.c_str())));
		else if (key == "NeuralDetailStrength") out.neural_detail_strength = clamp_neural_unit(static_cast<float>(std::atof(val.c_str())), 1.0f, kNeuralDetailMax);
		else if (key == "NeuralColourStrength") out.neural_colour_strength = clamp_neural_unit(static_cast<float>(std::atof(val.c_str())), 1.0f);
		else if (key == "NeuralModelPercent")
			out.neural_model_scale = neural_model_scale_index_for_percent(
				static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10)));
		else if (key == "NeuralModelResolution") {
			static const unsigned int kWasPercent[4] = { 100u, 80u, 50u, 40u };
			const unsigned int v = static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10));
			out.neural_model_scale = v < 4u
				? neural_model_scale_index_for_percent(kWasPercent[v])
				: kNeuralModelScaleDefault;
		}
		else if (key == "NeuralMode") {
			out.neural_mode = clamp_neural_mode(static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10)));
			have_mode = true;
		}
		else if (key == "NeuralRefreshBands")
			legacy_bands = std::strtoul(val.c_str(), nullptr, 10);
		else if (key == "NeuralBalanced")
			legacy_balanced = parse_bool(val);
		else if (key == "NeuralSmoothBlend")
			out.neural_smooth_blend = clamp_neural_unit(
				static_cast<float>(std::atof(val.c_str())), kNeuralBlendDefault);
		else if (key == "NeuralSmoothSteady")
			out.neural_smooth_steady = clamp_neural_range(
				static_cast<float>(std::atof(val.c_str())),
				kNeuralSteadyMin, kNeuralSteadyMax, kNeuralSteadyDefault);
		else if (key == "NeuralCarryReject")
			out.neural_carry_reject = clamp_neural_unit(
				static_cast<float>(std::atof(val.c_str())), kNeuralRejectDefault);
		else if (key == "NeuralTemporal") {
			const unsigned int v = static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10));
			out.neural_smooth_blend = clamp_neural_unit(
				(1.0f - neural_temporal_alpha_legacy(v)) / 0.95f, kNeuralBlendDefault);
		}
		else if (key == "NeuralPaperWhite")
			out.neural_paper_white = clamp_neural_range(static_cast<float>(std::atof(val.c_str())),
				kNeuralPaperWhiteMin, kNeuralPaperWhiteMax, 1.0f);
		else if (key == "NeuralHighlightGuard")
			out.neural_highlight_guard = clamp_neural_range(static_cast<float>(std::atof(val.c_str())),
				kNeuralHighlightGuardMin, kNeuralHighlightGuardMax, 2.0f);
		else if (key == "NeuralPasses") out.neural_passes = clamp_neural_passes(static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10)));
		else if (key == "NeuralNoMotionVectors") out.neural_no_motion_vectors = parse_bool(val);
		else if (key == "NeuralNoDepth") out.neural_no_depth = parse_bool(val);
		else if (key == "NeuralAutoMask") out.neural_auto_mask = parse_bool(val);
		else if (key == "NeuralSmoothing") out.neural_smoothing = parse_bool(val);
		else if (key == "NeuralHighlightGuardOn") out.neural_highlight_guard_on = parse_bool(val);
		else if (key == "ScreenshotIterations")
			out.accum_iterations = clamp_accum_iterations(
				static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10)));
		else if (key == "NeuralResetOnCut") out.neural_reset_on_cut = parse_bool(val);
		else if (key == "NeuralToggleKey") {
			const unsigned int v = static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10));
			out.neural_toggle_key = (v <= 0xFFu) ? v : 0u;
		}
		else if (key == "SplitCatmullRom") out.split_catmull_rom = parse_bool(val);
		else if (key == "JitterSceneRule") {
			const unsigned long v = std::strtoul(val.c_str(), nullptr, 10);
			out.jitter_scene_rule = v <= 2ul ? static_cast<unsigned int>(v) : 0u;
		}
		else if (key == "JitterTestedQuads") out.jitter_tested_quads = parse_bool(val);
		else if (key == "UpscalerCapture") out.upscaler_capture = parse_bool(val);
		else if (key == "ScopeCapture") out.scope_capture = parse_bool(val);
		else if (key == "ScopeFrames") {
			const unsigned long v = std::strtoul(val.c_str(), nullptr, 10);
			out.scope_frames = v >= 1ul && v <= kScopeFramesMax ? static_cast<unsigned int>(v) : kScopeFramesDefault;
		}
		else if (key == "ScopeDir") out.scope_dir = val;
		else if (key == "ScopeKey") {
			const unsigned long v = std::strtoul(val.c_str(), nullptr, 0);
			out.scope_key = v >= 1ul && v <= 0xFEul ? static_cast<unsigned int>(v) : kScopeKeyDefault;
		}
		else if (key == "JitterFault") out.jitter_fault = val;
		else if (key == "KeepInterface") out.keep_interface = parse_bool(val);
		else if (key == "ForceSystemMemory") out.force_system_memory = parse_bool(val);
		else if (key == "DebugInfo") out.debug_info = parse_bool(val);
		else if (key == "VerboseLog") out.verbose_log = parse_bool(val);
		else if (key == "InternalFlowQuality") out.internal_flow_quality = parse_bool(val) ? 1u : 0u;
		else if (key == "DebugView") {
			const unsigned int v = static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 10));
			out.debug_view = (v < kDebugViewCount) ? v : 0u;
		}
	}
	if (!have_mode && legacy_bands > 1ul)
		out.neural_mode = static_cast<unsigned int>(NeuralMode::UltraPerformance);
	else if (!have_mode && legacy_balanced)
		out.neural_mode = static_cast<unsigned int>(NeuralMode::Balanced);
}

void save_settings(const Settings &in, const std::wstring &ini_path)
{
	const std::string path = narrow(ini_path);
	std::ofstream out(path, std::ios::trunc);
	if (!out)
		return;
	out << "[AeonSR]\n";
	out << "Enabled=" << (in.enabled ? 1 : 0) << "\n";
	out << "ShowOsd=" << (in.show_osd ? 1 : 0) << "\n";
	out << "UpscaleEffects=" << (in.upscale_effects ? 1 : 0) << "\n";
	out << "RenderPreset=" << in.render_preset << "\n";
	out << "Sharpness=" << in.sharpness << "\n";
	out << "SpatialJitter=" << (in.spatial_jitter ? 1 : 0) << "\n";
	out << "UpscaleMode=" << in.upscale_mode << "\n";
	if (in.backend < kBackendChoiceCount)
		out << "Upscaler=" << in.backend << "\n";
	out << "VsrInputFormat=" << (in.vsr_input_format < kVsrInputFormatCount ? in.vsr_input_format : 0u) << "\n";
	out << "FsrProvider=" << in.fsr_provider << "\n";
	out << "NeuralRender=" << (in.neural_render ? 1 : 0) << "\n";
	out << "NeuralStyle=" << clamp_neural_style(in.neural_style) << "\n";
	out << "NeuralIntensity=" << clamp_neural_unit(in.neural_intensity, kNeuralIntensityDefault) << "\n";
	out << "NeuralLocalStructure=" << clamp_neural_unit(in.neural_local_structure, kNeuralStructureDefault, kNeuralExtendedMax) << "\n";
	out << "NeuralLocalTone=" << clamp_neural_unit(in.neural_local_tone, kNeuralToneDefault, kNeuralExtendedMax) << "\n";
	out << "NeuralSkinStructure=" << clamp_neural_skin(in.neural_skin_structure) << "\n";
	out << "NeuralDetailStrength=" << clamp_neural_unit(in.neural_detail_strength, 1.0f, kNeuralDetailMax) << "\n";
	out << "NeuralColourStrength=" << clamp_neural_unit(in.neural_colour_strength, 1.0f) << "\n";
	out << "NeuralModelPercent=" << neural_model_scale_percent(
		in.neural_model_scale < kNeuralModelScaleCount ? in.neural_model_scale : kNeuralModelScaleDefault)
		<< "\n";
	out << "NeuralSmoothBlend=" << clamp_neural_unit(in.neural_smooth_blend, kNeuralBlendDefault) << "\n";
	out << "NeuralSmoothSteady=" << clamp_neural_range(in.neural_smooth_steady,
		kNeuralSteadyMin, kNeuralSteadyMax, kNeuralSteadyDefault) << "\n";
	out << "NeuralCarryReject=" << clamp_neural_unit(in.neural_carry_reject, kNeuralRejectDefault) << "\n";
	out << "NeuralPaperWhite="
		<< clamp_neural_range(in.neural_paper_white, kNeuralPaperWhiteMin, kNeuralPaperWhiteMax, 1.0f) << "\n";
	out << "NeuralHighlightGuard="
		<< clamp_neural_range(in.neural_highlight_guard, kNeuralHighlightGuardMin, kNeuralHighlightGuardMax, 2.0f)
		<< "\n";

	out << "NeuralPasses=" << clamp_neural_passes(in.neural_passes) << "\n";
	out << "NeuralMode=" << clamp_neural_mode(in.neural_mode) << "\n";
	out << "NeuralNoMotionVectors=" << (in.neural_no_motion_vectors ? 1 : 0) << "\n";
	out << "NeuralNoDepth=" << (in.neural_no_depth ? 1 : 0) << "\n";
	out << "NeuralAutoMask=" << (in.neural_auto_mask ? 1 : 0) << "\n";
	out << "NeuralSmoothing=" << (in.neural_smoothing ? 1 : 0) << "\n";
	out << "NeuralHighlightGuardOn=" << (in.neural_highlight_guard_on ? 1 : 0) << "\n";
	out << "ScreenshotIterations=" << clamp_accum_iterations(in.accum_iterations) << "\n";
	out << "NeuralResetOnCut=" << (in.neural_reset_on_cut ? 1 : 0) << "\n";
	out << "NeuralToggleKey=" << (in.neural_toggle_key <= 0xFFu ? in.neural_toggle_key : 0u) << "\n";
	out << "SplitCatmullRom=" << (in.split_catmull_rom ? 1 : 0) << "\n";
	out << "JitterSceneRule=" << (in.jitter_scene_rule <= 2u ? in.jitter_scene_rule : 0u) << "\n";
	if (!in.jitter_tested_quads)
		out << "JitterTestedQuads=0\n";
	out << "UpscalerCapture=" << (in.upscaler_capture ? 1 : 0) << "\n";
	out << "ScopeCapture=" << (in.scope_capture ? 1 : 0) << "\n";
	out << "ScopeFrames=" << (in.scope_frames >= 1u && in.scope_frames <= kScopeFramesMax
		? in.scope_frames : kScopeFramesDefault) << "\n";
	if (!in.scope_dir.empty())
		out << "ScopeDir=" << in.scope_dir << "\n";
	if (in.scope_key != kScopeKeyDefault)
		out << "ScopeKey=" << in.scope_key << "\n";
	if (!in.jitter_fault.empty())
		out << "JitterFault=" << in.jitter_fault << "\n";
	out << "KeepInterface=" << (in.keep_interface ? 1 : 0) << "\n";
	out << "ForceSystemMemory=" << (in.force_system_memory ? 1 : 0) << "\n";
	out << "MvProbe=" << (in.mv_probe ? 1 : 0) << "\n";
	if (in.gpu_timings)
		out << "GpuTimings=1\n";
	out << "DebugInfo=" << (in.debug_info ? 1 : 0) << "\n";
	out << "VerboseLog=" << (in.verbose_log ? 1 : 0) << "\n";
	out << "DebugView=" << (in.debug_view < kDebugViewCount ? in.debug_view : 0u) << "\n";
	out << "InternalFlowQuality=" << (in.internal_flow_quality != 0u ? 1 : 0) << "\n";
}

}

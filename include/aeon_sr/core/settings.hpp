#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace aeon_sr {

inline constexpr float kSpatialScaleMin = 0.50f;
inline constexpr float kSpatialScaleMax = 1.00f;
inline constexpr float kSpatialScaleDefault = 0.88f;

float clamp_spatial_scale(float scale) noexcept;

inline constexpr unsigned int kDebugViewCount = 5u;

void scaled_render_size(uint32_t display_w, uint32_t display_h, float scale,
	uint32_t *out_w, uint32_t *out_h) noexcept;

enum class UpscaleMode : unsigned int {
	Dlaa = 0,
	UltraQuality,
	Quality,
	Balanced,
	Performance,
	UltraPerformance,
};

enum class VsrInputFormat : unsigned int { Auto = 0, Rgb = 1, Ayuv = 2, Nv12 = 3 };
inline constexpr unsigned int kVsrInputFormatCount = 4u;

inline constexpr float kJitterAmount = 1.0f;

inline constexpr unsigned int kScopeFramesDefault = 48u;
inline constexpr unsigned int kScopeFramesMax = 1024u;
inline constexpr unsigned int kScopeKeyDefault = 0x7Au;

float vsr_scale_for_mode(UpscaleMode mode) noexcept;

inline constexpr unsigned int kFunctionalRenderPresets[] = { 0u, 5u, 6u, 10u, 11u, 12u, 13u };

unsigned int normalize_render_preset(unsigned int preset) noexcept;

inline constexpr unsigned int kNeuralStyleCount = 3u;
inline constexpr float kNeuralIntensityDefault = 1.0f;

inline constexpr float kNeuralStructureDefault = 1.0f;
inline constexpr float kNeuralToneDefault = 1.0f;

inline constexpr float kNeuralSkinMin = -1.0f;
inline constexpr float kNeuralSkinMax = 1.0f;
inline constexpr float kNeuralSkinDefault = kNeuralSkinMin;

inline constexpr float kNeuralUnitMax = 1.0f;

inline constexpr float kNeuralExtendedMax = 2.0f;

inline constexpr unsigned int kNeuralPassMax = 3u;
unsigned int clamp_neural_passes(unsigned int passes) noexcept;

inline constexpr float kNeuralPassFixedCost = 1.3f;
inline constexpr float kNeuralPassPixelCost = 15.8f;

float neural_chain_relative_cost(float model_scale, unsigned int passes, float row_fraction = 1.0f) noexcept;

enum class NeuralMode : unsigned int { Quality = 0u, Balanced = 1u, Performance = 2u, UltraPerformance = 3u };
inline constexpr unsigned int kNeuralModeCount = 4u;
unsigned int clamp_neural_mode(unsigned int mode) noexcept;
inline constexpr float kNeuralModeKeep[kNeuralModeCount] = { 1.0f, 0.75f, 0.6f, 0.5f };
inline constexpr float kNeuralModeKeepMin = 0.4f;
unsigned int neural_mode_width(unsigned int frame_w, float scale, unsigned int mode) noexcept;
double neural_warp_to_frame(double model_uv, double a) noexcept;
double neural_warp_to_model(double frame_uv, double a) noexcept;

float clamp_neural_unit(float v, float fallback, float max_value = kNeuralUnitMax) noexcept;

float clamp_neural_skin(float v, float max_value = kNeuralUnitMax) noexcept;

float clamp_neural_range(float v, float lo, float hi, float fallback) noexcept;

inline constexpr float kNeuralModelScales[] = {
	1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f };
inline constexpr unsigned int kNeuralModelScaleCount = 7u;
inline constexpr unsigned int kNeuralModelScaleDefault = 4u;
unsigned int neural_model_scale_index_for_percent(unsigned int percent) noexcept;
unsigned int neural_model_scale_percent(unsigned int index) noexcept;

unsigned int neural_model_width(unsigned int frame_w, float scale) noexcept;
float neural_model_scale_factor(unsigned int index) noexcept;

inline constexpr float kNeuralBlendDefault = 0.3f;
inline constexpr float kNeuralSteadyMin = 1.0f;
inline constexpr float kNeuralSteadyMax = 4.0f;
inline constexpr float kNeuralSteadyDefault = 2.5f;
inline constexpr float kNeuralRejectDefault = 0.7f;

inline constexpr float kNeuralInputDetail = 0.2f;

float neural_blend_alpha(float blend) noexcept;
float neural_steady_weight(float band, float blend) noexcept;
float neural_reject_threshold(float reject) noexcept;

float neural_scale_compensation(unsigned int index) noexcept;

inline constexpr float kNeuralDetailMax = 2.0f;
inline constexpr float kNeuralPaperWhiteMin = 0.05f;
inline constexpr float kNeuralPaperWhiteMax = 16.0f;
inline constexpr float kNeuralHighlightGuardMin = 1.0f;
inline constexpr float kNeuralHighlightGuardMax = 8.0f;
inline constexpr float kNeuralHighlightGuardOff = 1.0e6f;
inline constexpr unsigned int kAccumMin = 2u;
inline constexpr unsigned int kAccumMax = 64u;
inline constexpr unsigned int kAccumDefault = 8u;
unsigned int clamp_accum_iterations(unsigned int n) noexcept;

inline constexpr unsigned int kNeuralDebugViewCount = 4u;
unsigned int clamp_neural_style(unsigned int style) noexcept;

struct Settings {
	bool enabled = false;
	bool show_osd = false;

	unsigned int render_preset = 11;
	float sharpness = 0.25f;

	bool upscale_effects = false;

	bool mv_probe = true;

	bool gpu_timings = false;

	bool spatial_jitter = true;

	unsigned int upscale_mode = 0;

	inline static constexpr unsigned int kBackendUnset = 0xFFFFFFFFu;
	unsigned int backend = kBackendUnset;

	unsigned int vsr_input_format = 0;

	std::string fsr_provider;

	unsigned int jitter_scene_rule = 0;
	bool jitter_tested_quads = true;
	bool depth_regrid = true;
	bool split_catmull_rom = false;
	bool upscaler_capture = false;
	bool scope_capture = false;
	unsigned int scope_frames = kScopeFramesDefault;
	std::string scope_dir;
	unsigned int scope_key = kScopeKeyDefault;
	std::string jitter_fault;
	bool keep_interface = true;
	bool force_system_memory = false;

	bool neural_render = false;
	unsigned int neural_style = 0;

	float neural_intensity = kNeuralIntensityDefault;
	float neural_local_structure = kNeuralStructureDefault;
	float neural_local_tone = kNeuralToneDefault;

	float neural_skin_structure = kNeuralSkinDefault;

	float neural_detail_strength = 1.0f;

	float neural_colour_strength = 1.0f;

	unsigned int neural_model_scale = kNeuralModelScaleDefault;

	bool neural_smoothing = true;
	float neural_smooth_blend = kNeuralBlendDefault;
	float neural_smooth_steady = kNeuralSteadyDefault;
	float neural_carry_reject = kNeuralRejectDefault;

	float neural_paper_white = 1.0f;

	bool neural_highlight_guard_on = false;
	float neural_highlight_guard = 2.0f;

	unsigned int neural_debug_view = 0;

	bool neural_auto_mask = true;

	bool neural_reset_on_cut = false;

	unsigned int neural_passes = 1u;
	unsigned int neural_mode = 0u;
	bool neural_no_motion_vectors = false;

	bool neural_no_depth = false;

	unsigned int neural_toggle_key = 0x75u;

	unsigned int accum_iterations = kAccumDefault;

	bool debug_info = false;

	bool verbose_log = false;

	unsigned int debug_view = 0;
};

void load_settings(Settings &out, const std::wstring &ini_path);
void save_settings(const Settings &in, const std::wstring &ini_path);

inline constexpr const char *kAddonFileName =
	sizeof(void *) == 8 ? "AeonSR.addon64" : "AeonSR.addon32";
inline constexpr const wchar_t *kAddonFileNameW =
	sizeof(void *) == 8 ? L"AeonSR.addon64" : L"AeonSR.addon32";

std::wstring module_directory(HMODULE module);
std::wstring join_path(const std::wstring &dir, const wchar_t *file);

}

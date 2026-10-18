#include "aeon_sr/core/panel.hpp"
#include "aeon_sr/core/diag_report.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/i18n.hpp"
#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/core/panel_widgets.hpp"
#include "aeon_sr/ngx/ngx_dlssnr.hpp"

#include <dxgiformat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace aeon_sr {
namespace panel {
namespace {

std::string build_date(const char *stamp)
{
	static const char *const kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	unsigned int day = 0, year = 0;
	char month[4]{};
	if (stamp == nullptr || std::sscanf(stamp, "%3s %u %u", month, &day, &year) != 3)
		return stamp != nullptr ? stamp : "";
	for (unsigned int m = 0; m < 12; ++m)
		if (std::strcmp(month, kMonths[m]) == 0) {
			char out[16];
			std::snprintf(out, sizeof(out), "%04u-%02u-%02u", year, m + 1, day);
			return out;
		}
	return stamp;
}

std::string narrow_lossy(const std::wstring &ws)
{
	std::string out;
	out.reserve(ws.size());
	for (wchar_t c : ws)
		out.push_back(c < 128 ? static_cast<char>(c) : '?');
	return out;
}

}

const char *format_label(uint32_t fmt)
{
	switch (static_cast<DXGI_FORMAT>(fmt)) {
	case DXGI_FORMAT_R32_FLOAT: return "R32F";
	case DXGI_FORMAT_R16_FLOAT: return "R16F";
	case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
	case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
	case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8 sRGB";
	case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8 sRGB";
	case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16";
	case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
	case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
	case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8T";
	case DXGI_FORMAT_R32_TYPELESS: return "R32T";
	case DXGI_FORMAT_R16_TYPELESS: return "R16T";
	case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
	case DXGI_FORMAT_D32_FLOAT: return "D32F";
	case DXGI_FORMAT_UNKNOWN: return "-";
	default: return "other";
	}
}

namespace {

void hint(const char *text)
{
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		return;
	ui::tooltip_alpha_begin();
	ImGui::SetTooltip("%s", text);
	ui::tooltip_alpha_end();
}

void technical_hint(const std::string &tech);

void note(const char *fmt, ...) IM_FMTARGS(1);
void note(const char *fmt, ...)
{
	char text[2048];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	ui::wrapped(ui::palette().muted, text);
}

void line(const char *fmt, ...) IM_FMTARGS(1);
void line(const char *fmt, ...)
{
	char text[2048];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	ui::wrapped(ui::palette().text, text);
}

ImVec4 status_colour(UpscalerStatus s)
{
	const ui::Palette &pal = ui::palette();
	switch (s) {
	case UpscalerStatus::Ready: return pal.ok;
	case UpscalerStatus::Idle: return pal.off;
	case UpscalerStatus::Loading: return pal.loading;
	case UpscalerStatus::NeedMotionVectors:
	case UpscalerStatus::NeedDepth: return pal.warn;
	default: return pal.fail;
	}
}

bool status_failed(UpscalerStatus s)
{
	switch (s) {
	case UpscalerStatus::Ready:
	case UpscalerStatus::Idle:
	case UpscalerStatus::Loading:
	case UpscalerStatus::NeedMotionVectors:
	case UpscalerStatus::NeedDepth: return false;
	default: return true;
	}
}

ImVec4 neural_verdict_colour(const PanelState &state, const Settings &settings);

bool upscaler_off(const Settings &settings)
{
	return !settings.enabled || settings.backend == static_cast<unsigned>(BackendChoice::None);
}

bool upscaler_lit(const PanelState &state, const Settings &settings)
{
	return !upscaler_off(settings) && !status_failed(state.active_status);
}

void status_line(const ImVec4 &colour, const char *word, bool see_diagnostics)
{
	const float gap = ImGui::CalcTextSize(" ").x;
	ImGui::AlignTextToFramePadding();
	if (ImGui::CalcTextSize(TR(st_status_prefix)).x + gap + ui::pill_width(word) <= ui::avail_width()) {
		ImGui::TextDisabled("%s", TR(st_status_prefix));
		ImGui::SameLine(0.0f, gap);
	}
	ui::pill(colour, word);
	if (!see_diagnostics)
		return;
	const char *pointer = TR(st_see_diagnostics);
	const float w = ImGui::CalcTextSize(pointer).x;
	const float pill_x = ImGui::GetItemRectMin().x;
	const float space = gap * 1.5f;
	const float right = ImGui::GetCursorScreenPos().x + ui::avail_width();
	if (ImGui::GetItemRectMax().x + space + w <= right) {
		ImGui::SameLine(0.0f, space);
		ImGui::AlignTextToFramePadding();
	} else {
		const ImVec2 c = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(pill_x, c.y - ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}
	ImGui::TextDisabled("%s", pointer);
}

struct RowOption {
	const char *label;
	const char *tip = nullptr;
	ui::Brand brand = ui::Brand::Neutral;
};

const char *visible_end(const char *s)
{
	while (*s != '\0' && !(s[0] == '#' && s[1] == '#'))
		++s;
	return s;
}

constexpr ImGuiHoveredFlags kRowHover = ImGuiHoveredFlags_AllowWhenDisabled;

void row_tooltip(const char *opt_tip, const char *row_tip)
{
	if (opt_tip == nullptr && row_tip == nullptr)
		return;
	ui::tooltip_alpha_begin();
	if (ImGui::BeginTooltip()) {
		if (opt_tip != nullptr)
			ImGui::TextUnformatted(opt_tip);
		if (opt_tip != nullptr && row_tip != nullptr)
			ImGui::Separator();
		if (row_tip != nullptr)
			ImGui::TextUnformatted(row_tip);
		ImGui::EndTooltip();
	}
	ui::tooltip_alpha_end();
}

bool button_row_n(const char *caption, int *idx, const RowOption *opts, int n, const char *row_tip,
	bool glow = false, bool lit = true, bool grid = false)
{
	const int N = n;
	const ImGuiStyle &style = ImGui::GetStyle();
	const float left = ImGui::GetCursorScreenPos().x;
	const float k = ImGui::GetFontSize() / 13.0f;
	const char *caption_end = visible_end(caption);
	const float caption_w = caption_end != caption ? ImGui::CalcTextSize(caption, caption_end).x : 0.0f;
	if (caption_w > 0.0f)
		ui::note_caption(caption_w);

	const float pad = style.FramePadding.x;
	const float min_w = ImGui::GetFrameHeight() * 2.0f;
	std::vector<float> text(static_cast<size_t>(N));
	float text_sum = 0.0f, natural_sum = 0.0f;
	for (int i = 0; i < N; ++i) {
		const size_t u = static_cast<size_t>(i);
		text[u] = ImGui::CalcTextSize(opts[i].label, nullptr, true).x;
		text_sum += text[u];
		natural_sum += std::max(text[u] + pad * 2.0f, min_w);
	}
	const auto fits_line = [&](float room) {
		return natural_sum <= room || (room - text_sum) / (2.0f * static_cast<float>(N)) >= pad * 0.6f;
	};
	const auto fits_grid = [&](float room) {
		for (int lines = 2; lines <= N; ++lines) {
			const int cols = (N + lines - 1) / lines;
			if (cols < 2 || (N + cols - 1) / cols != lines)
				continue;
			std::vector<float> column(static_cast<size_t>(cols), 0.0f);
			for (int i = 0; i < N; ++i)
				column[static_cast<size_t>(i % cols)] = std::max(column[static_cast<size_t>(i % cols)],
					std::max(text[static_cast<size_t>(i)] + pad * 2.0f, min_w));
			float total = 0.0f;
			for (float c : column)
				total += c;
			if (total <= room)
				return true;
		}
		return false;
	};
	const bool fits_beside = natural_sum <= ui::control_width() || (grid && fits_grid(ui::control_width()));
	if (caption_w > 0.0f && !fits_beside)
		ui::need_stacked_captions();
	const bool caption_beside = caption_w > 0.0f && fits_beside && !ui::stacked_captions();
	const bool caption_above = caption_w > 0.0f && !caption_beside;
	const float room = std::floor(caption_beside ? ui::control_width() : ui::avail_width());
	const bool wrap = grid ? natural_sum > room : !fits_line(room);

	std::vector<float> widths(static_cast<size_t>(N));
	int per = N;
	if (!wrap) {
		const bool squeezed = natural_sum > room;
		const float p = squeezed ? (room - text_sum) / (2.0f * static_cast<float>(N)) : pad;
		float sum = 0.0f, widest = 0.0f;
		for (int i = 0; i < N; ++i) {
			const size_t u = static_cast<size_t>(i);
			widths[u] = std::max(text[u] + p * 2.0f, squeezed ? 0.0f : min_w);
			sum += widths[u];
			widest = std::max(widest, widths[u]);
		}
		const float even = room / static_cast<float>(N);
		for (float &w : widths)
			w = even >= widest ? even : w + (room - sum) / static_cast<float>(N);
	} else {
		std::vector<float> column;
		per = 0;
		for (int lines = 2; lines <= N && per == 0; ++lines) {
			const int cols = (N + lines - 1) / lines;
			if ((N + cols - 1) / cols != lines)
				continue;
			column.assign(static_cast<size_t>(cols), 0.0f);
			for (int i = 0; i < N; ++i)
				column[static_cast<size_t>(i % cols)] = std::max(column[static_cast<size_t>(i % cols)],
					std::max(text[static_cast<size_t>(i)] + pad * 2.0f, min_w));
			float total = 0.0f;
			for (float c : column)
				total += c;
			if (total <= room || cols == 1)
				per = cols;
		}
		float total = 0.0f, widest = 0.0f;
		for (float c : column) {
			total += c;
			widest = std::max(widest, c);
		}
		const float even = room / static_cast<float>(per);
		for (float &c : column)
			c = even >= widest ? even : c + (room - total) / static_cast<float>(per);
		for (int i = 0; i < N; ++i)
			widths[static_cast<size_t>(i)] = column[static_cast<size_t>(i % per)];
	}
	std::vector<float> starts(static_cast<size_t>(N));
	std::vector<int> line_of(static_cast<size_t>(N));
	{
		float x = 0.0f;
		for (int i = 0; i < N; ++i) {
			const size_t u = static_cast<size_t>(i);
			if (i % per == 0)
				x = 0.0f;
			line_of[u] = i / per;
			const float a = std::floor(left + x);
			x += widths[u];
			const bool line_end = (i + 1) % per == 0 || i == N - 1;
			const float b = line_end && (!wrap || (i + 1) % per == 0) ? std::floor(left + room) : std::floor(left + x);
			starts[u] = a;
			widths[u] = b - a;
		}
	}

	bool changed = false;
	bool tip_shown = false;
	ImGui::PushID(caption);
	ImGui::BeginGroup();
	if (caption_above) {
		ImGui::TextUnformatted(caption, caption_end);
		const ImVec2 c = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(c.x, c.y - style.ItemSpacing.y + 2.0f * k));
	}

	const float h = ImGui::GetFrameHeight();
	const float top = ImGui::GetCursorScreenPos().y;
	if (wrap) {
		const int lines = line_of[static_cast<size_t>(N) - 1] + 1;
		ui::segment_frame(ImVec2(std::floor(left), top), ImVec2(std::floor(left + room), top + h * static_cast<float>(lines)),
			h, lines);
	}

	const ui::Brand chosen = *idx >= 0 && *idx < N ? opts[*idx].brand : ui::Brand::Neutral;
	if (glow && lit && ui::brand_colour(chosen) != nullptr) {
		const size_t s = static_cast<size_t>(*idx);
		const float y = top + static_cast<float>(line_of[s]) * h;
		ui::segment_glow(ImVec2(starts[s], y), ImVec2(starts[s] + widths[s], y + h), *ui::brand_colour(chosen));
	}

	for (int i = 0; i < N; ++i) {
		const size_t u = static_cast<size_t>(i);
		const bool first = i == 0 || line_of[u - 1] != line_of[u];
		const bool last = i == N - 1 || line_of[u + 1] != line_of[u];
		if (!first)
			ImGui::SameLine(0.0f, 0.0f);
		else
			ImGui::SetCursorScreenPos(ImVec2(starts[u], top + h * static_cast<float>(line_of[u])));
		const ImDrawFlags corners = first && last ? ImDrawFlags_RoundCornersAll
			: first ? ImDrawFlags_RoundCornersLeft
			: last ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;

		const bool selected = (i == *idx);
		ImGui::PushID(i);
		const bool pressed = ui::segment(opts[i].label, selected, widths[u], corners,
			!first && !selected && i - 1 != *idx, opts[i].brand, lit, !wrap, glow);
		ImGui::PopID();

		if (!tip_shown && ImGui::IsItemHovered(kRowHover)) {
			row_tooltip(opts[i].tip, row_tip);
			tip_shown = true;
		}
		if (pressed && !selected) {
			*idx = i;
			changed = true;
		}
	}

	if (caption_beside) {
		if (wrap)
			ImGui::SetCursorScreenPos(ImVec2(std::floor(left + room) + style.ItemInnerSpacing.x, top));
		else
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(caption, caption_end);
	}
	ImGui::EndGroup();

	if (!tip_shown && ImGui::IsItemHovered(kRowHover))
		row_tooltip(nullptr, row_tip);
	ImGui::PopID();
	if (caption_above) {
		const ImVec2 c = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + 3.0f * k - style.ItemSpacing.y));
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
	}
	return changed;
}

template <int N>
bool button_row(const char *caption, int *idx, const RowOption (&opts)[N], const char *row_tip, bool glow = false,
	bool lit = true, bool grid = false)
{
	static_assert(N > 0, "a row needs at least one option");
	return button_row_n(caption, idx, opts, N, row_tip, glow, lit, grid);
}

template <unsigned int Count, int N>
bool button_row_index(const char *caption, unsigned int *field, const RowOption (&opts)[N], const char *row_tip,
	bool glow = false, bool lit = true, bool grid = false)
{
	static_assert(N == static_cast<int>(Count), "one option per value the field can hold");
	int idx = *field < Count ? static_cast<int>(*field) : -1;
	if (!button_row(caption, &idx, opts, row_tip, glow, lit, grid))
		return false;
	*field = static_cast<unsigned int>(idx);
	return true;
}

template <typename T, int N, int M>
bool button_row_value(const char *caption, T *field, const RowOption (&opts)[N], const T (&values)[M], const char *row_tip,
	bool lit = true)
{
	static_assert(N == M, "labels and values must stay in step");
	int idx = -1;
	for (int i = 0; i < M; ++i)
		if (values[i] == *field) { idx = i; break; }
	if (!button_row(caption, &idx, opts, row_tip, false, lit))
		return false;
	*field = values[idx];
	return true;
}

void draw_jitter_numbers(const PanelState &state, PanelActions &actions)
{
	const auto pair = [](const char *label, bool known, float x, float y, const char *tip) {
		if (known)
			ImGui::Text("%s  %+.3f, %+.3f", label, x, y);
		else
			ImGui::TextDisabled("%s  -", label);
		hint(tip);
	};
	ImGui::Text("This frame: %s", state.scope_plan.c_str());
	hint("How the upscaler took this frame: drawn, split, empty, resampled or off.");
	pair("Computed", state.scope_computed_known, state.scope_computed_x, state.scope_computed_y,
		"The offset the add-on asked the game to draw this frame at.");
	pair("Drawn   ", state.scope_drawn_known, state.scope_drawn_x, state.scope_drawn_y,
		"The offset of the arm the game's draws carried, as read back; where their pixels landed is the scope capture's to measure.");
	pair("Told    ", state.scope_told_known, state.scope_told_x, state.scope_told_y,
		"The offset the upscaler was told, in screen pixels.");
	ImGui::Text("Armed %llu, drawn %llu", static_cast<unsigned long long>(state.scope_armed_serial),
		static_cast<unsigned long long>(state.scope_drawn_serial));
	hint("Which offset was armed and which one the frame came back with; they match when nothing lags.");
	ImGui::Text("Last second: %u frames, %u late, %u empty", state.scope_second_frames, state.scope_lag_frames,
		state.scope_empty_frames);
	hint("Late: drawn with an older offset than armed. Empty: nothing drawn took the offset.");
	if (!state.scope_residual_x.empty()) {
		const int n = static_cast<int>(state.scope_residual_x.size());
		ImGui::PlotLines("##told_x", state.scope_residual_x.data(), n, 0, "told - drawn, x", -0.5f, 0.5f,
			ImVec2(0.0f, 40.0f));
		hint("Told minus the read-back offset, per frame: moves when the upscaler is told something else than the arm the draws carried. A draw that lands elsewhere does not show here; the scope capture measures that.");
		ImGui::PlotLines("##told_y", state.scope_residual_y.data(), n, 0, "told - drawn, y", -0.5f, 0.5f,
			ImVec2(0.0f, 40.0f));
		hint("Told minus the read-back offset, per frame: moves when the upscaler is told something else than the arm the draws carried. A draw that lands elsewhere does not show here; the scope capture measures that.");
	}
	if (state.scope_fault != "none" && !state.scope_fault.empty())
		ImGui::TextColored(ui::palette().warn, "Fault injected: %s", state.scope_fault.c_str());
	if (state.scope_capture_enabled) {
		if (ui::button(ui::icon::kTarget, "Capture jitter scope"))
			actions.scope_capture = true;
		hint("Saves the next frames at every stage, lossless, for tools/jitter_scope.");
		if (!state.scope_status.empty())
			ImGui::TextWrapped("Scope: %s", narrow_lossy(state.scope_status).c_str());
	}
}

void draw_quality_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	const bool idle = !upscaler_lit(state, settings);
	ui::subsection(TR(sub_image));
	{
		const ui::Dim dim(idle);
		ui::slider_float(TR(img_sharpness), &settings.sharpness, 0.0f, kSharpnessMax, "%.2f",
			ImGuiSliderFlags_AlwaysClamp);
		hint(TR(img_sharpness_tip));
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		if (state.active_is_dlss) {
			constexpr ui::Brand nv = ui::Brand::Nvidia;
			const RowOption kPresetOptions[] = {
				{ TR(opt_default), TR(img_preset_default_tip), nv },
				{ "E", TR(img_preset_older_tip), nv },
				{ "F", TR(img_preset_older_tip), nv },
				{ "J", TR(img_preset_j_tip), nv },
				{ "K", TR(img_preset_k_tip), nv },
				{ "L", TR(img_preset_l_tip), nv },
				{ "M", TR(img_preset_m_tip), nv },
			};
			if (button_row_value(TR(img_dlss_preset), &settings.render_preset, kPresetOptions, kFunctionalRenderPresets,
					TR(img_dlss_preset_tip), !idle)) {
				actions.reset = true;
				actions.persist = true;
			}
		}
	}

	ui::subsection(TR(sub_reconstruction));
	{
		const ui::Dim dim(idle);
		if (ui::toggle(TR(rec_jitter), &settings.spatial_jitter)) {
			actions.reset = true;
			actions.invalidate_history = true;
			actions.persist = true;
		}
		hint(TR(rec_jitter_tip));
	}

	if (settings.spatial_jitter && state.jitter_refused)
		note("%s", TR(rec_jitter_refused));
	const ui::Dim dim(idle && !(settings.neural_render && state.neural_last == 1));
	if (ui::toggle(TR(rec_upscale_effects), &settings.upscale_effects)) {
		actions.reset = true;
		actions.invalidate_history = true;
		actions.persist = true;
	}
	hint(TR(rec_upscale_effects_tip));
	if (ui::button(ui::icon::kUndo, TR(rec_reset_history)))
		actions.reset = true;
	hint(TR(rec_reset_history_tip));
}

void draw_neural_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	ui::band_begin("##neural_band");
	if (ui::toggle(TR(nr_enable), &settings.neural_render)) {
		if (!settings.neural_render)
			actions.neural_restart = true;
		actions.persist = true;
	}
	hint(TR(nr_enable_tip));

	const bool nvidia = vendor_may_be_nvidia(state.gpu_vendor);
	const bool gpu_refused = nvidia &&
		(state.neural_adapter_unsupported || state.neural_feature_unsupported ||
			!state.neural_runtime_serves_card ||
			state.neural_status == UpscalerStatus::UnsupportedGpu);
	const ui::Palette &pal = ui::palette();
	const bool running = settings.neural_render && state.neural_last == 1;
	const ImVec4 verdict = settings.neural_render ? neural_verdict_colour(state, settings) : pal.off;
	const bool failed = verdict.x == pal.fail.x && verdict.y == pal.fail.y && verdict.z == pal.fail.z;
	const char *word = running ? TR(st_running)
		: !failed ? TR(st_not_running)
		: state.neural_driver_too_old || !state.neural_initialized ? TR(st_could_not_start) : TR(st_failed);
	if (!nvidia || gpu_refused)
		status_line(verdict, TR(nr_not_available_gpu), true);
	else if (settings.neural_render)
		status_line(verdict, word, !running);
	ui::band_end(!settings.neural_render);
	if (!settings.neural_render)
		return;
	const bool faded = !nvidia || gpu_refused || (!running && failed);

	ui::subsection(TR(sub_performance));
	{
		const ui::Dim dim(faded);
		const RowOption kScaleOptions[] = {
			{ TR(nr_scale_full), TR(nr_scale_full_tip) },
			{ "90%", "" },
			{ "80%", TR(nr_scale_80_tip) },
			{ "70%", "" },
			{ "60%", "" },
			{ "50%", "" },
			{ "40%", TR(nr_scale_40_tip) },
		};
		if (button_row_index<kNeuralModelScaleCount>(TR(nr_resolution), &settings.neural_model_scale,
				kScaleOptions,
				TR(nr_resolution_tip), false, running))
			actions.persist = true;

		constexpr ui::Brand nv = ui::Brand::Nvidia;
		const RowOption kModeOptions[] = {
			{ TR(mode_quality), TR(nr_mode_quality_tip), nv },
			{ TR(mode_balanced), TR(nr_mode_balanced_tip), nv },
			{ TR(mode_performance), TR(nr_mode_performance_tip), nv },
			{ TR(mode_ultra_performance), TR(nr_mode_ultra_tip), nv },
		};
		if (button_row_index<kNeuralModeCount>(TR(nr_mode), &settings.neural_mode, kModeOptions,
				TR(nr_mode_tip), false, running))
			actions.persist = true;

		if (ui::toggle(TR(nr_no_depth), &settings.neural_no_depth)) {
			actions.neural_restart = true;
			actions.persist = true;
		}
		hint(TR(nr_no_depth_tip));

		const unsigned int cost_mode = clamp_neural_mode(settings.neural_mode);
		float rows = 1.0f;
		if (cost_mode != 0u && state.neural_area_share > 0.0f) {
			rows = state.neural_area_share;
		} else if (cost_mode != 0u) {
			const uint32_t fw = state.upscaler_out_width != 0u ? state.upscaler_out_width : 2560u;
			const uint32_t fh = state.upscaler_out_width != 0u ? state.upscaler_out_height : 1440u;
			const float sc = neural_model_scale_factor(settings.neural_model_scale);
			const float k = static_cast<float>(neural_mode_width(fw, sc, cost_mode, fh)) /
				static_cast<float>((std::max)(neural_mode_width(fw, sc, 0u, fh), 1u));
			rows = k * k;
		}
		const float rel = neural_chain_relative_cost(
			neural_model_scale_factor(settings.neural_model_scale),
			settings.neural_passes, rows);
		if (running) {
			if (state.neural_gpu_ms > 0.0f) {
				char ms[32];
				std::snprintf(ms, sizeof(ms), "%.2f ms", state.neural_gpu_ms);
				const std::string line_text = TR(nr_cost_line);
				const size_t at = line_text.find("%s");
				const std::string before = line_text.substr(0, at);
				const std::string after = at == std::string::npos ? std::string() : line_text.substr(at + 2);
				const ui::Span cost[] = { { before.c_str(), pal.muted }, { ms, pal.text },
					{ after.c_str(), pal.muted } };
				ui::flow(cost, IM_ARRAYSIZE(cost));
				hint(TR(nr_cost_tip));
			}
			note(TR(nr_cost_estimate), rel);
			hint(TR(nr_cost_estimate_tip));
		}
	}

	ui::subsection(TR(sub_look));
	{
		const ui::Dim dim(faded);
		const RowOption kStyleOptions[] = {
			{ TR(nr_style_standard), TR(nr_style_standard_tip) },
			{ TR(nr_style_natural), TR(nr_style_natural_tip) },
			{ TR(nr_style_cinematic), TR(nr_style_cinematic_tip) },
		};
		if (button_row_index<kNeuralStyleCount>(TR(nr_style), &settings.neural_style, kStyleOptions,
				TR(nr_style_tip), false, running))
			actions.persist = true;
		ui::slider_float(TR(nr_detail), &settings.neural_detail_strength,
			0.0f, kNeuralDetailMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		hint(TR(nr_detail_tip));
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		ui::slider_float(TR(nr_colour), &settings.neural_colour_strength,
			0.0f, kNeuralUnitMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		hint(TR(nr_colour_tip));
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		ui::slider_float(TR(nr_intensity), &settings.neural_intensity, 0.0f, kNeuralUnitMax, "%.2f",
			ImGuiSliderFlags_AlwaysClamp);
		hint(TR(nr_intensity_tip));
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		ui::slider_float(TR(nr_structure), &settings.neural_local_structure, 0.0f, kNeuralExtendedMax, "%.2f",
			ImGuiSliderFlags_AlwaysClamp);
		hint(TR(nr_structure_tip));
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		ui::slider_float(TR(nr_tone), &settings.neural_local_tone, 0.0f, kNeuralExtendedMax, "%.2f",
			ImGuiSliderFlags_AlwaysClamp);
		hint(TR(nr_tone_tip));
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		ui::slider_float(TR(nr_skin), &settings.neural_skin_structure,
			kNeuralSkinMin, kNeuralSkinMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		hint(TR(nr_skin_tip));
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			settings.neural_skin_structure = clamp_neural_skin(settings.neural_skin_structure);
			actions.persist = true;
		}
		if (ui::toggle(TR(nr_auto_mask), &settings.neural_auto_mask))
			actions.persist = true;
		hint(TR(nr_auto_mask_tip));
	}

	ui::subsection(TR(sub_stability));
	{
		const ui::Dim dim(faded);
		if (ui::toggle(TR(nr_smoothing), &settings.neural_smoothing))
			actions.persist = true;
		hint(TR(nr_smoothing_tip));
		if (ui::toggle(TR(nr_no_motion), &settings.neural_no_motion_vectors)) {
			actions.neural_restart = true;
			actions.persist = true;
		}
		hint(TR(nr_no_motion_tip));
		if (ui::toggle(TR(nr_reset_on_cut), &settings.neural_reset_on_cut))
			actions.persist = true;
		hint(TR(nr_reset_on_cut_tip));
	}

	ui::subsection(TR(sub_hdr));
	{
		const ui::Dim dim(faded);
		ui::slider_float(TR(nr_paper_white), &settings.neural_paper_white,
			kNeuralPaperWhiteMin, kNeuralPaperWhiteMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		hint(TR(nr_paper_white_tip));
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		if (ui::toggle(TR(nr_highlight_guard), &settings.neural_highlight_guard_on))
			actions.persist = true;
		hint(TR(nr_highlight_guard_tip));
	}

	ui::subsection(TR(sub_tools));
	{
		const ui::Dim dim(faded);
		const RowOption kDebugOptions[] = {
			{ TR(opt_off), nullptr },
			{ TR(nr_debug_input), TR(nr_debug_input_tip) },
			{ TR(nr_debug_output), TR(nr_debug_output_tip) },
			{ TR(nr_debug_difference), TR(nr_debug_difference_tip) },
		};
		button_row_index<kNeuralDebugViewCount>(TR(nr_debug_view), &settings.neural_debug_view, kDebugOptions,
			TR(nr_debug_view_tip), false, running);

		const RowOption kKeyOptions[] = {
			{ TR(opt_none), nullptr }, { "F5", nullptr }, { "F6", nullptr }, { "F7", nullptr }, { "F8", nullptr },
		};
		static const unsigned int kKeyCodes[] = { 0u, 0x74u, 0x75u, 0x76u, 0x77u };
		if (button_row_value(TR(nr_toggle_key), &settings.neural_toggle_key, kKeyOptions, kKeyCodes,
				TR(nr_toggle_key_tip), running))
			actions.persist = true;

		if (ui::button(ui::icon::kRefresh, TR(nr_restart))) {
			actions.neural_restart = true;
			actions.persist = true;
		}
		hint(TR(nr_restart_tip));
		if (state.scope_capture_enabled) {
			ui::same_line_if_fits(ui::button_width(ui::icon::kCamera, "Capture neural frame"));
			if (ui::button(ui::icon::kCamera, "Capture neural frame"))
				actions.neural_capture = true;
			hint("Saves a few uncompressed frames to the captures folder beside the\n"
				"add-on. Use these to compare image quality - a screen recording\n"
				"adds noise of its own.");
			if (!state.neural_capture_status.empty())
				line("Capture: %s", narrow_lossy(state.neural_capture_status).c_str());
		}
	}

	ui::subsection(TR(sub_advanced));
	{
		const ui::Dim dim(faded);
		static const RowOption kPassOptions[] = { { "1x", nullptr }, { "2x", nullptr }, { "3x", nullptr } };
		static const unsigned int kPassValues[] = { 1u, 2u, 3u };
		static_assert(IM_ARRAYSIZE(kPassValues) == static_cast<int>(kNeuralPassMax),
			"the row must offer every pass count up to kNeuralPassMax");
		if (button_row_value((std::string(TR(nr_passes)) + "##chain").c_str(), &settings.neural_passes, kPassOptions, kPassValues,
				TR(nr_passes_tip), running))
			actions.persist = true;

		if (clamp_neural_passes(settings.neural_passes) > 1u &&
			settings.neural_model_scale > 1u)
			ui::calloutf(pal.warn, ui::icon::kWarn, TR(nr_passes_wasted));
	}
}

#if AEONSR_FRAME_GENERATION
void draw_framegen_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	if (ui::toggle(TR(fg_enable), &settings.frame_generation))
		actions.persist = true;
	hint(TR(fg_enable_tip));

	const FrameGenReadout &f = state.framegen;
	const bool running = settings.frame_generation && f.generating;
	const RowOption kGeneratorOptions[] = {
		{ "AeonSR", TR(fg_aeonsr_tip), ui::Brand::Neutral },
		{ "FSR", TR(fg_fsr_tip), ui::Brand::Amd },
		{ "XeSS", TR(fg_xess_tip), ui::Brand::Intel },
		{ "DLSS", TR(fg_dlss_tip), ui::Brand::Nvidia },
	};
	if (button_row_index<kFrameGeneratorCount>(TR(fg_generator), &settings.frame_generator, kGeneratorOptions,
			TR(fg_generator_tip), false, running))
		actions.persist = true;

	const RowOption kMultiplyOptions[] = {
		{ TR(opt_auto), TR(fg_auto_tip) },
		{ "2x", nullptr }, { "3x", nullptr }, { "4x", nullptr }, { "5x", nullptr }, { "6x", nullptr },
	};
	static const unsigned int kMultiplyValues[] = { 0u, 2u, 3u, 4u, 5u, 6u };
	static_assert(IM_ARRAYSIZE(kMultiplyValues) == static_cast<int>(kFrameGenerationMultiplierMax),
		"the row must offer Auto and every multiplier up to kFrameGenerationMultiplierMax");
	if (button_row_value(TR(fg_multiply), &settings.frame_generation_multiplier, kMultiplyOptions, kMultiplyValues,
			TR(fg_multiply_tip),
			running && !f.through_game_present))
		actions.persist = true;
	if (running && f.through_game_present)
		note("%s", TR(fg_through_present));
	if (running && f.own_window && f.own_asked >= 2 && f.own_steps_max >= 1 && f.own_asked > f.own_steps_max)
		note(TR(fg_goes_up_to), frame_generator_label(f.generator), f.own_steps_max);

	int limit = static_cast<int>(settings.frame_generation_limit);
	if (ui::slider_int(TR(fg_limit), &limit, 0, 240, limit == 0 ? TR(fg_limit_off) : "%d fps")) {
		settings.frame_generation_limit = static_cast<unsigned int>(std::clamp(limit, 0, 240));
	}
	if (ImGui::IsItemDeactivatedAfterEdit())
		actions.persist = true;
	if (running && f.own_window && f.even_cap_fps != 0) {
		char cap[1024];
		std::snprintf(cap, sizeof cap, TR(fg_cap_even_tip),
			f.even_cap_fps, f.screen_hz);
		hint(cap);
	} else {
		hint(TR(fg_cap_tip));
	}

	if (settings.frame_generation) {
		if (!f.status.empty())
		{
			std::string tech;
			line("%s", diag_said(narrow_lossy(f.status), i18n::current(), i18n::S::fg_status_generic, &tech).c_str());
			technical_hint(tech);
		}
		if (f.real_fps > 0.0) {
			if (f.own_window && f.added_latency_ms > 0.0)
				line(TR(fg_rates_delay), f.real_fps, f.shown_fps, f.added_latency_ms);
			else
				line(TR(fg_rates), f.real_fps, f.shown_fps);
			std::string more;
			char b[1024];
			if (f.generating && f.own_window && f.own_steps == 1) {
				more += TR(fg_nothing_made);
			} else if (f.generating) {
				const std::string by = std::string(frame_generator_label(f.generator)) +
					(f.generator_version.empty() ? "" : " ") + narrow_lossy(f.generator_version);
				if (f.own_window && f.own_steps >= 2)
					std::snprintf(b, sizeof b, f.own_auto ? TR(fg_made_by_at_auto) : TR(fg_made_by_at), by.c_str(), f.own_steps);
				else if (f.own_window)
					std::snprintf(b, sizeof b, TR(fg_made_by_every), by.c_str());
				else
					std::snprintf(b, sizeof b, TR(fg_made_by), by.c_str());
				more += b;
			}
			if (f.own_window && f.screen_latency_ms > 0.0 && f.added_latency_ms > 0.0) {
				std::snprintf(b, sizeof b, TR(fg_screen_latency), f.screen_latency_ms, f.added_latency_ms);
				more += "\n";
				more += b;
			}
			if (f.own_window) {
				more += "\n";
				more += TR(fg_counter_note);
			}
			if (!f.route.empty()) {
				std::snprintf(b, sizeof b, TR(diag_technical), f.route.c_str());
				more += "\n";
				more += b;
			}
			if (!more.empty())
				hint(more.c_str());
		}
	}

	ui::subsection(TR(sub_pacing));
	if (ui::toggle(TR(fg_even_pacing), &settings.pace_to_display))
		actions.persist = true;
	hint(TR(fg_even_pacing_tip));

	const RowOption kLatencyOptions[] = {
		{ TR(opt_off), TR(fg_latency_off_tip), ui::Brand::Neutral },
		{ "AeonSR", TR(fg_latency_aeonsr_tip), ui::Brand::Neutral },
		{ "Reflex", TR(fg_latency_reflex_tip), ui::Brand::Nvidia },
	};
	if (button_row_index<3>(TR(fg_low_latency), &settings.low_latency, kLatencyOptions,
			TR(fg_low_latency_tip)))
		actions.persist = true;
}
#endif

void draw_experiments_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	ui::subsection(TR(sub_upscaler_input));
	const RowOption kUpscaleOptions[] = {
		{ TR(mode_native), TR(exp_native_tip) },
		{ TR(mode_ultra_quality), TR(exp_ultra_quality_tip) },
		{ TR(mode_quality) },
		{ TR(mode_balanced) },
		{ TR(mode_performance) },
		{ TR(mode_ultra_performance) },
	};
	constexpr unsigned int kUpscaleModeCount = static_cast<unsigned int>(UpscaleMode::UltraPerformance) + 1u;
	{
		const ui::Dim dim(upscaler_off(settings));
		if (button_row_index<kUpscaleModeCount>(TR(exp_mode), &settings.upscale_mode, kUpscaleOptions,
				TR(exp_mode_tip),
				false, true, true)) {
			actions.reset = true;
			actions.invalidate_history = true;
			actions.persist = true;
		}
	}
	if (upscaler_off(settings))
		note("%s", TR(exp_needs_upscaler));
	const GameScaleVerdict verdict = state.game_scale_verdict;
	const bool wrong_api = verdict == GameScaleVerdict::WrongApi;
	if (state.game_integrated) {
		const ui::Dim dim(upscaler_off(settings) || wrong_api);
		bool scale_on = settings.game_render_scale;
		if (ui::toggle(TR(exp_game_scale), &scale_on) && (!wrong_api || !scale_on)) {
			settings.game_render_scale = scale_on;
			actions.reset = true;
			actions.invalidate_history = true;
			actions.persist = true;
		}
		hint(TR(exp_game_scale_integrated_tip));
	}
	if (state.game_integrated) {
		if (!settings.game_render_scale)
			note(TR(exp_supported_turn_on),
				state.game_title.c_str());
		else if (upscaler_off(settings))
			note(TR(exp_no_upscaler_own), state.game_title.c_str());
		else if (state.game_scale_render_w == 0 && !state.game_note.empty())
		{
			std::string tech;
			note(TR(exp_game_note), state.game_title.c_str(),
				diag_said(narrow_lossy(state.game_note), i18n::current(), i18n::S::exp_game_note_generic, &tech).c_str());
			technical_hint(tech);
		}
		else
			note(TR(exp_game_draws_mode), state.game_title.c_str());
		if (settings.game_render_scale && state.game_scale_render_w != 0)
			note(TR(exp_drawn_at), state.game_scale_render_w,
				state.game_scale_render_h, state.game_scale_w, state.game_scale_h);
	} else if (!state.game_title.empty()) {
		std::string tech;
		note(TR(exp_left_to_game), state.game_title.c_str(),
			diag_said(narrow_lossy(state.game_note), i18n::current(), i18n::S::exp_game_note_generic, &tech).c_str());
		technical_hint(tech);
	}

	ui::subsection(TR(sub_neural_screenshot));
	{
		const RowOption kAccumOptions[] = {
			{ "2x", nullptr }, { "4x", nullptr }, { "8x", TR(exp_accum_8x_tip) },
			{ "16x", nullptr }, { "32x", nullptr }, { "64x", TR(exp_accum_64x_tip) },
		};
		static const unsigned int kAccumValues[] = { 2u, 4u, 8u, 16u, 32u, 64u };
		const bool neural_running = settings.neural_render && state.neural_last == 1;
		if (button_row_value((std::string(TR(exp_accum_passes)) + "##screenshot").c_str(), &settings.accum_iterations,
				kAccumOptions, kAccumValues,
				TR(exp_accum_passes_tip), neural_running))
			actions.persist = true;

		const bool can_run = neural_running;
		ImGui::BeginDisabled(!can_run && !state.accum_running);
		if (!state.accum_running) {
			if (ui::button(ui::icon::kPlay, TR(exp_accum_start)))
				actions.accum_start = true;
		} else if (ui::button(ui::icon::kStop, TR(exp_accum_release))) {
			actions.accum_stop = true;
		}
		ImGui::EndDisabled();
		hint(TR(exp_accum_tip));

		const ui::Palette &pal = ui::palette();
		if (state.accum_running && !state.accum_holding) {
			char progress[512];
			std::snprintf(progress, sizeof(progress), TR(exp_accum_progress),
				state.accum_done, state.accum_target);
			ImGui::ProgressBar(state.accum_target != 0
					? static_cast<float>(state.accum_done) / static_cast<float>(state.accum_target) : 0.0f,
				ImVec2(ui::avail_width(), 0.0f), progress);
		} else if (state.accum_holding) {
			ui::calloutf(pal.ok, ui::icon::kOk,
				TR(exp_accum_held), state.accum_done);
		} else if (!can_run) {
			note("%s", TR(exp_accum_needs_neural));
		}
	}
}

ImVec4 check_colour(CheckState s)
{
	const ui::Palette &pal = ui::palette();
	switch (s) {
	case CheckState::Ok: return pal.ok;
	case CheckState::Warn: return pal.warn;
	case CheckState::Fail: return pal.fail;
	case CheckState::Off: break;
	}
	return pal.off;
}

ImVec4 neural_verdict_colour(const PanelState &state, const Settings &settings)
{
	for (const DiagCheck &c : diag_checks(state, settings))
		if (std::string(c.name) == "Neural rendering")
			return check_colour(c.state);
	return ui::palette().off;
}

const char *check_state_text(CheckState s)
{
	switch (s) {
	case CheckState::Ok: return TR(check_ok);
	case CheckState::Warn: return TR(check_warn);
	case CheckState::Fail: return TR(check_fail);
	case CheckState::Off: break;
	}
	return TR(check_off);
}

const char *check_icon(CheckState s)
{
	switch (s) {
	case CheckState::Ok: return ui::icon::kOk;
	case CheckState::Warn: return ui::icon::kWarn;
	case CheckState::Fail: return ui::icon::kFail;
	case CheckState::Off: break;
	}
	return ui::icon::kOff;
}

bool room_beside(float fixed, int columns)
{
	const float cells = ImGui::GetStyle().CellPadding.x * 2.0f * static_cast<float>(columns);
	return ui::avail_width() - fixed - cells >= ImGui::CalcTextSize("0").x * 28.0f;
}

void technical_hint(const std::string &tech)
{
	if (tech.empty())
		return;
	char tip[2048];
	std::snprintf(tip, sizeof(tip), TR(diag_technical), tech.c_str());
	hint(tip);
}

void draw_health(const PanelState &state, const Settings &settings, bool skip_log)
{
	std::vector<DiagCheck> checks = diag_checks(state, settings, i18n::current());
	if (skip_log)
		checks.erase(std::remove_if(checks.begin(), checks.end(),
			[](const DiagCheck &c) { return std::string(c.name) == "Log"; }), checks.end());
	float name_w = 0.0f;
	for (const DiagCheck &c : checks)
		name_w = std::max(name_w, ImGui::CalcTextSize(c.title).x);
	ui::tray_begin("##health_tray");
	const bool beside = room_beside(name_w + ImGui::GetFontSize() * 1.2f, 3);
	if (!ImGui::BeginTable("##health", beside ? 3 : 2,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX,
			ImVec2(ui::avail_width(), 0.0f))) {
		ui::tray_end();
		return;
	}
	ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed);
	if (beside)
		ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn("##detail", ImGuiTableColumnFlags_WidthStretch);
	for (const DiagCheck &c : checks) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		const char *g = ui::glyph(check_icon(c.state));
		if (*g != '\0') {
			ui::icon_text(check_colour(c.state), g);
			hint(check_state_text(c.state));
		} else {
			ImGui::TextColored(check_colour(c.state), "[%s]", check_state_text(c.state));
		}
		ImGui::TableNextColumn();
		if (beside) {
			ImGui::TextUnformatted(c.title);
			ImGui::TableNextColumn();
		}
		if (!beside)
			ImGui::TextColored(ui::palette().label, "%s", c.title);
		std::string tech;
		line("%s", diag_player_text(c.detail, &tech).c_str());
		technical_hint(c.tech.empty() ? tech : tech.empty() ? c.tech : c.tech + "\n" + tech);
		if (!c.action.empty())
			note("%s", c.action.c_str());
	}
	ImGui::EndTable();
	ui::tray_end();
}

void draw_report_tools(const PanelState &state, Settings &settings, PanelActions &actions)
{
	if (ui::button(ui::icon::kSave, TR(diag_save_report)))
		actions.save_report = true;
	hint(TR(diag_save_report_tip));
	ui::same_line_if_fits(ui::button_width(ui::icon::kCopy, TR(diag_copy_report)));
	if (ui::button(ui::icon::kCopy, TR(diag_copy_report)))
		actions.copy_report = true;
	hint(TR(diag_copy_report_tip));

	if (!state.report_error.empty()) {
		ui::callout(ui::palette().fail, ui::icon::kAlert, TR(diag_report_failed));
		technical_hint(narrow_lossy(state.report_error));
	}
	else if (!state.report_path.empty())
		note(TR(diag_report_path), narrow_lossy(state.report_path).c_str());

	if (ui::toggle(TR(diag_detailed_log), &settings.verbose_log))
		actions.persist = true;
	hint(TR(diag_detailed_log_tip));
	if (!state.log_path.empty())
		note(TR(diag_log_path), narrow_lossy(state.log_path).c_str());
}

std::string problem_source(const std::string &tag, i18n::Lang lang = i18n::Lang::en_US)
{
	std::string key;
	for (char c : tag)
		if (std::isalnum(static_cast<unsigned char>(c)))
			key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	using i18n::S;
	static const struct { const char *key; S name; } kNames[] = {
		{ "neural", S::hn_neural }, { "neuralrender", S::hn_neural },
		{ "host", S::src_helper_process }, { "remote", S::src_helper_process }, { "debugview", S::hn_motion_depth_view },
		{ "flow", S::hn_motion }, { "probe", S::src_motion_probe }, { "interop", S::src_bridge }, { "shader", S::src_shaders },
		{ "upscaler", S::hn_upscaler }, { "framegen", S::fg_enable }, { "gpu", S::hn_gpu }, { "report", S::sub_report },
		{ "jitter", S::rec_jitter }, { "latency", S::fg_low_latency }, { "interface", S::src_interface },
		{ "game", S::src_game }, { "scale", S::src_scale }, { "files", S::src_files }, { "crash", S::src_crash },
		};
	static const struct { const char *key, *name; } kProper[] = {
		{ "dlss", "DLSS" }, { "vsr", "RTX VSR" }, { "d3d12", "Direct3D 12" }, { "hud", "HUD" }, { "aeonsr", "Aeon SR" },
	};
	for (const auto &n : kNames)
		if (key == n.key)
			return i18n::text(lang, n.name);
	for (const auto &n : kProper)
		if (key == n.key)
			return n.name;
	static const struct { const char *prefix; S name; } kOpenings[] = {
		{ "jitter", S::rec_jitter }, { "interop", S::src_bridge }, { "nativedlss", S::hn_game_dlss },
		{ "ngxlayer", S::hn_other_program }, { "ngx", S::hn_upscaler }, { "depth", S::hn_depth },
		{ "interface", S::src_interface }, { "game", S::src_game }, { "framegen", S::fg_enable },
		{ "pacing", S::fg_enable }, { "swapchain", S::fg_enable }, { "flight", S::sub_report },
		{ "neural", S::hn_neural }, { "flow", S::hn_motion },
	};
	for (const auto &n : kOpenings)
		if (key.rfind(n.prefix, 0) == 0)
			return i18n::text(lang, n.name);
	return "Aeon SR";
}

std::vector<DiagEntry> recent_problems(const PanelState &state, const Settings &settings)
{
	std::vector<std::string> flagged;
	for (const DiagCheck &c : diag_checks(state, settings))
		if (c.state == CheckState::Warn || c.state == CheckState::Fail)
			flagged.push_back(c.name);
	std::vector<DiagEntry> problems;
	for (const DiagEntry &e : diag_recent(8, DiagLevel::Warn))
		if (std::find(flagged.begin(), flagged.end(), problem_source(e.tag)) == flagged.end())
			problems.push_back(e);
	return problems;
}

std::string log_count(const PanelState &state, const Settings &settings)
{
	for (const DiagCheck &c : diag_checks(state, settings, i18n::current()))
		if (std::string(c.name) == "Log")
			return c.detail;
	return std::string();
}

void draw_recent_problems(const std::vector<DiagEntry> &problems, const std::string &count)
{
	if (problems.empty())
		return;
	ui::subsection(TR(sub_recent_problems), count.c_str());
	const ui::Palette &pal = ui::palette();
	const char *warn_icon = ui::glyph(ui::icon::kWarn);
	const char *fail_icon = ui::glyph(ui::icon::kFail);
	const float gap = ImGui::CalcTextSize(" ").x;
	const float icon_w = std::max(ImGui::CalcTextSize(warn_icon).x, ImGui::CalcTextSize(fail_icon).x);
	float tag_w = 0.0f;
	for (const DiagEntry &e : problems)
		tag_w = std::max(tag_w, ImGui::CalcTextSize(problem_source(e.tag, i18n::current()).c_str()).x);
	if (icon_w > 0.0f)
		tag_w += icon_w + gap;
	ui::tray_begin("##problems_tray");
	const bool beside = room_beside(ImGui::CalcTextSize("000.0s").x + tag_w, 3);
	if (!ImGui::BeginTable("##problems", beside ? 3 : 2,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX,
			ImVec2(ui::avail_width(), 0.0f))) {
		ui::tray_end();
		return;
	}
	ImGui::TableSetupColumn("##time", ImGuiTableColumnFlags_WidthFixed);
	if (beside)
		ImGui::TableSetupColumn("##tag", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn("##text", ImGuiTableColumnFlags_WidthStretch);
	const auto source = [&](const DiagEntry &e) {
		const bool error = e.level == DiagLevel::Error;
		const char *g = error ? fail_icon : warn_icon;
		if (*g != '\0') {
			ui::icon_text(error ? pal.fail : pal.warn, g);
			ImGui::SameLine(0.0f, gap);
		}
		ImGui::TextUnformatted(problem_source(e.tag, i18n::current()).c_str());
	};
	for (auto it = problems.rbegin(); it != problems.rend(); ++it) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextDisabled("%.1fs", static_cast<double>(it->time_s));
		hint(TR(diag_seconds_tip));
		ImGui::TableNextColumn();
		if (beside) {
			source(*it);
			ImGui::TableNextColumn();
		}
		if (!beside)
			source(*it);
		std::string tech, cut;
		const std::string said = diag_said(narrow_lossy(it->text), i18n::current(),
			it->level == DiagLevel::Error ? i18n::S::rp_error : i18n::S::rp_warning, &tech);
		line("%s", diag_player_text(said, &cut).c_str());
		technical_hint(tech.empty() ? cut : tech);
	}
	ImGui::EndTable();
	ui::tray_end();
}

void draw_neural_details(const PanelState &state, const Settings &settings)
{
	if (!settings.neural_render)
		return;
	ui::subsection(TR(hn_neural));
	if (state.neural_dll_found && !state.neural_runtime_file.empty()) {
		const size_t cut = state.neural_runtime_file.find_last_of(L"\\/");
		const std::string name = narrow_lossy(cut == std::wstring::npos
			? state.neural_runtime_file : state.neural_runtime_file.substr(cut + 1));
		if (state.neural_runtime_serves_card)
			note(TR(nd_runtime_built_for), name.c_str(),
				state.neural_runtime_cards.empty() ? TR(nd_unknown_cards)
					: diag_cards_text(state.neural_runtime_cards, i18n::current()).c_str());
		else
			note(TR(nd_runtime), name.c_str());
		hint(TR(nd_runtime_tip));
	}
	if (state.neural_last == 1 && state.neural_model_width != 0) {
		if (state.neural_eval_rows != 0 && state.neural_eval_rows < state.neural_model_height)
			note(TR(nd_working_at_share), state.neural_model_width,
				state.neural_model_height, 100.0 * state.neural_eval_rows / state.neural_model_height);
		else
			note(TR(nd_working_at), state.neural_model_width, state.neural_model_height);
	}
	if (!state.neural_driver_too_old && !state.neural_adapter_unsupported) {
		const bool arch = state.neural_requirement_known && state.neural_min_architecture != 0;
		const bool driver = state.neural_required_driver_major != 0;
		if (arch && driver)
			note(TR(nd_needs_arch_driver),
				diag_arch_text(state.neural_min_architecture, i18n::current()),
				state.neural_required_driver_major, state.neural_required_driver_minor);
		else if (arch)
			note(TR(nd_needs_arch), diag_arch_text(state.neural_min_architecture, i18n::current()));
		else if (driver)
			note(TR(nd_needs_driver),
				state.neural_required_driver_major, state.neural_required_driver_minor);
	}
	note(TR(nd_colour_space),
		state.neural_color_space == NeuralColorSpace::Pq ? "HDR10 (PQ)"
		: state.neural_color_space == NeuralColorSpace::ScrgbLinear ? "scRGB (linear)"
		: "SDR / sRGB");
	note(TR(nd_passes_run), static_cast<unsigned long long>(state.neural_eval_count));
}

void draw_diagnostics_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	const std::vector<DiagEntry> problems = recent_problems(state, settings);
	ui::subsection(TR(sub_health));
	draw_health(state, settings, !problems.empty());

	if (ui::toggle(TR(diag_show_osd), &settings.show_osd))
		actions.persist = true;
	hint(TR(diag_show_osd_tip));

	ui::subsection(TR(sub_report));
	draw_report_tools(state, settings, actions);

	draw_recent_problems(problems, log_count(state, settings));

	if (!ui::disclosure("Details", TR(sub_details)))
		return;

	note(TR(diag_build), build_date(state.build_stamp).c_str());
	note(TR(diag_camera_motion), state.last_motion_px);
	if (state.jitter_in_frame && state.jitter_still) {
		note("%s", TR(diag_jitter_still));
	} else if (state.jitter_in_frame && state.jitter_mixed) {
		note("%s", TR(diag_jitter_mixed));
	} else if (state.jitter_in_frame && state.jitter_aliased_draws != 0) {
		note(TR(diag_jitter_aliased),
			state.jitter_moved_draws, state.jitter_moved_draws + state.jitter_plain_draws,
			state.jitter_aliased_draws);
	} else if (state.jitter_in_frame) {
		note(TR(diag_jitter_drawn),
			state.jitter_moved_draws, state.jitter_moved_draws + state.jitter_plain_draws);
	} else if (settings.spatial_jitter && state.jitter_withheld) {
		note("%s", state.jitter_active ? TR(diag_jitter_withheld_scaled) : TR(diag_jitter_withheld));
	} else if (settings.spatial_jitter && !state.jitter_refused && !state.jitter_active) {
		note("%s", TR(diag_jitter_idle));
	} else if (settings.spatial_jitter && !state.jitter_refused && state.jitter_note[0] != '\0') {
		std::string tech;
		note(TR(diag_jitter_resampled), diag_said(state.jitter_note, i18n::current(), i18n::S::diag_jitter_generic, &tech).c_str());
		technical_hint(tech);
	} else if (settings.spatial_jitter && !state.jitter_refused) {
		note("%s", TR(diag_jitter_waiting));
	}
	if (state.scope_capture_enabled && state.scope_have && ImGui::TreeNode("Jitter numbers")) {
		draw_jitter_numbers(state, actions);
		ImGui::TreePop();
	}
	if (ui::toggle(TR(diag_probe), &settings.mv_probe)) {
		if (!settings.mv_probe)
			actions.probe_reset = true;
		actions.persist = true;
	}
	hint(TR(diag_probe_tip));
	if (settings.mv_probe && state.probe_unsupported)
		ui::callout(ui::palette().fail, ui::icon::kAlert,
			TR(diag_probe_unsupported));

	if (state.active_is_fsr) {
		const std::wstring &running = state.fsr_provider_version;
		const bool running_4 = running.size() > 1 && running[0] == L'4' && running[1] == L'.';
		bool offers_4 = false;
		for (const std::string &p : state.fsr_providers)
			offers_4 = offers_4 || (p.size() > 1 && p[0] == '4' && p[1] == '.');

		if (running.empty())
			note("%s", TR(diag_fsr_unnamed));
		if (offers_4 && !running_4) {
			ui::calloutf(ui::palette().warn, ui::icon::kWarn,
				TR(diag_fsr_fell_back),
				running.empty() ? TR(diag_fsr_older) : narrow_lossy(running).c_str());
			hint(TR(diag_fsr_fell_back_tip));
		}
	} else if (state.active_is_vsr) {
		note(TR(diag_vsr_line),
			state.vsr_capable ? TR(diag_vsr_capable) : TR(diag_vsr_not_capable),
			state.vsr_enabled ? TR(diag_vsr_on) : TR(diag_vsr_off),
			state.vsr_in_use ? TR(diag_vsr_in_use) : TR(diag_vsr_not_in_use), state.vsr_level);
	}

	draw_neural_details(state, settings);

	ui::subsection(TR(sub_motion_depth_view));
	const RowOption kDebugViewOptions[] = {
		{ TR(opt_off) },
		{ TR(mdv_flow), TR(mdv_flow_tip) },
		{ TR(mdv_arrows), TR(mdv_arrows_tip) },
		{ TR(mdv_confidence), TR(mdv_confidence_tip) },
		{ TR(mdv_depth), TR(mdv_depth_tip) },
	};
	if (button_row_index<kDebugViewCount>("##Motion and depth view", &settings.debug_view, kDebugViewOptions,
			TR(mdv_tip)))
		actions.persist = true;
	note("%s", TR(mdv_drawn_note));
	if (settings.debug_view != 0) {
		const char *stage = TR(mdv_starting);
		switch (state.debug_view_stage) {
		case 1: stage = TR(mdv_drawing); break;
		case -1: stage = TR(mdv_failed_args); break;
		case -2: stage = TR(mdv_failed_compile); break;
		case -3: stage = TR(mdv_failed_format); break;
		case -4: stage = TR(mdv_failed_target); break;
		case -5: stage = TR(mdv_failed_context); break;
		case -6: stage = TR(mdv_waiting); break;
		case -7: stage = TR(mdv_failed_rejected); break;
		case -8: stage = TR(mdv_failed_copy_view); break;
		case -9: stage = TR(mdv_failed_memory); break;
		default: break;
		}
		const bool ok = state.debug_view_stage == 1;
		const bool waiting = state.debug_view_stage == -6;
		const ui::Palette &pal = ui::palette();
		status_line(ok ? pal.ok : waiting ? pal.warn : pal.fail, stage, false);
		if (state.debug_view_note != nullptr && state.debug_view_note[0] != 0)
			technical_hint(state.debug_view_note);
	}

	ui::subsection(TR(sub_about));
	note("Powered by NVIDIA DLSS and the NVIDIA NGX SDK.");
	note("NVIDIA, NVIDIA RTX and GeForce RTX are trademarks of NVIDIA Corporation.");
	note("AMD FidelityFX Super Resolution (c) Advanced Micro Devices, Inc. - MIT.");
	note("Intel XeSS (c) Intel Corporation - Intel Simplified Software License.");
	note("%s", TR(about_terms));
	ui::disclosure_end();
}

struct BackendOptions {
	RowOption o[5];
};

BackendOptions backend_options()
{
	return { { { "DLSS", TR(taa_dlss_tip), ui::Brand::Nvidia },
		{ "FSR", TR(taa_fsr_tip), ui::Brand::Amd },
		{ "XeSS", TR(taa_xess_tip), ui::Brand::Intel },
		{ TR(opt_none), TR(taa_none_tip), ui::Brand::None },
		{ "RTX VSR", TR(taa_vsr_tip), ui::Brand::Nvidia } } };
}

ui::Brand chosen_brand(const Settings &settings)
{
	if (!settings.enabled || settings.backend >= static_cast<unsigned>(IM_ARRAYSIZE(backend_options().o)))
		return ui::Brand::Neutral;
	return backend_options().o[settings.backend].brand;
}

ui::Brand active_brand(const PanelState &state, const Settings &settings)
{
	if (state.optiscaler_upscales)
		return ui::Brand::None;
	if (state.active_is_fsr)
		return ui::Brand::Amd;
	if (state.active_is_xess)
		return ui::Brand::Intel;
	if (state.active_is_dlss || state.active_is_vsr)
		return ui::Brand::Nvidia;
	return chosen_brand(settings);
}

const char *active_short_name(const PanelState &state, const Settings &settings)
{
	if (upscaler_off(settings))
		return nullptr;
	if (state.optiscaler_upscales)
		return "OptiScaler";
	if (state.active_is_fsr)
		return "FSR";
	if (state.active_is_xess)
		return "XeSS";
	if (state.active_is_vsr)
		return "RTX VSR";
	if (state.active_is_dlss)
		return "DLSS";
	return settings.backend < static_cast<unsigned>(IM_ARRAYSIZE(backend_options().o))
		? backend_options().o[settings.backend].label : nullptr;
}

}

void draw_taa_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	ui::band_begin("##taa_band");
	bool enabled = settings.enabled;
	if (ui::toggle(TR(taa_enable), &enabled)) {
		settings.enabled = enabled;
		actions.reset = true;
		actions.persist = true;
	}
	hint(TR(taa_enable_tip));

	if (button_row_index<kBackendChoiceCount>("##upscaler", &settings.backend, backend_options().o,
			TR(taa_which_tip), true, upscaler_lit(state, settings))) {
		actions.persist = true;
		actions.reset = true;
	}

	if (upscaler_off(settings))
		status_line(ui::palette().off, TR(st_off), false);
	else if (!state.upscaler_label.empty() || state.active_name != nullptr)
		status_line(status_colour(state.active_status), diag_status_text(state.active_status, i18n::current()),
			!state.active_error.empty());

	if (state.optiscaler_offered) {
		bool to_opti = settings.optiscaler_upscale;
		if (ui::toggle(TR(taa_optiscaler), &to_opti)) {
			settings.optiscaler_upscale = to_opti;
			if (to_opti) {
				settings.enabled = true;
				settings.backend = static_cast<unsigned>(BackendChoice::Dlss);
			} else if (settings.backend == static_cast<unsigned>(BackendChoice::Dlss) && !state.dlss_without_layer) {
				settings.backend = static_cast<unsigned>(BackendChoice::Fsr);
			}
			actions.persist = true;
			actions.reset = true;
		}
		hint(TR(taa_optiscaler_tip));
		if (state.optiscaler_held == PanelState::OptiscalerHeld::PickDlss)
			note("%s", TR(taa_optiscaler_pick_dlss));
		else if (state.optiscaler_held == PanelState::OptiscalerHeld::GameDlss)
			note("%s", TR(taa_optiscaler_game_dlss));
	}

	if (state.active_is_fsr && state.fsr_providers.size() > 1) {
		std::vector<RowOption> opts;
		opts.push_back(RowOption{ TR(opt_auto), TR(taa_fsr_auto_tip), ui::Brand::Amd });
		for (const std::string &pv : state.fsr_providers)
			opts.push_back(RowOption{ pv.c_str(), nullptr, ui::Brand::Amd });
		int idx = 0;
		for (size_t i = 0; i < state.fsr_providers.size(); ++i) {
			if (state.fsr_providers[i] == settings.fsr_provider)
				idx = static_cast<int>(i) + 1;
		}
		if (button_row_n(TR(taa_fsr_version), &idx, opts.data(), static_cast<int>(opts.size()),
				TR(taa_fsr_version_tip), false, upscaler_lit(state, settings))) {
			settings.fsr_provider = idx == 0 ? std::string() : state.fsr_providers[static_cast<size_t>(idx) - 1];
			actions.persist = true;
			actions.reset = true;
		}
	}
	ui::band_end();

	draw_quality_section(state, settings, actions);
}

void draw_overlay(const PanelState &state, Settings &settings, PanelActions &actions)
{
	const ui::Theme theme;
	char subtitle[64];
	std::snprintf(subtitle, sizeof(subtitle), "v%s - Barbatos AWLS", state.version != nullptr ? state.version : "");
	const bool upscaler_running = upscaler_lit(state, settings);
	ui::banner("AEON SR", subtitle);
	if (ui::section("Temporal Anti-Aliasing", ui::icon::kTaa, TR(sec_taa), true, nullptr, nullptr,
			active_short_name(state, settings), active_brand(state, settings), upscaler_running, true)) {
		draw_taa_section(state, settings, actions);
		ui::section_end();
	}
	const bool neural_running = settings.neural_render && state.neural_last == 1;
	if (ui::section("Neural Rendering", ui::icon::kNeural, TR(sec_neural), false, nullptr, nullptr,
			"DLSS 5", ui::Brand::Nvidia, neural_running)) {
		draw_neural_section(state, settings, actions);
		ui::section_end();
	}
#if AEONSR_FRAME_GENERATION
	static const char *const kFgShort[kFrameGeneratorCount] = { "AeonSR", "FSR", "XeSS", "DLSS" };
	const FrameGenerator fg_maker = settings.frame_generation && state.framegen.generating ? state.framegen.generator
		: static_cast<FrameGenerator>(settings.frame_generator);
	const ui::Brand fg_brand = fg_maker == FrameGenerator::Dlss ? ui::Brand::Nvidia
		: fg_maker == FrameGenerator::Fsr ? ui::Brand::Amd
		: fg_maker == FrameGenerator::Xess ? ui::Brand::Intel : ui::Brand::Neutral;
	if (ui::section("Frame generation (experimental)", ui::icon::kFrames, TR(sec_framegen), false,
			nullptr, nullptr, settings.frame_generation ? kFgShort[static_cast<unsigned int>(fg_maker) % kFrameGeneratorCount]
				: nullptr, fg_brand,
			settings.frame_generation && state.framegen.generating, true)) {
		draw_framegen_section(state, settings, actions);
		ui::section_end();
	}
#endif
	if (ui::section("Experiments", ui::icon::kExperiments, TR(sec_experiments), false)) {
		draw_experiments_section(state, settings, actions);
		ui::section_end();
	}
	{
		const CheckState worst = diag_worst(diag_checks(state, settings));
		const ImVec4 badge_colour = check_colour(worst);
		const char *badge = worst == CheckState::Fail ? TR(badge_something_failed)
			: worst == CheckState::Warn ? TR(badge_check_this) : nullptr;
		const char *badge_short = worst == CheckState::Fail ? TR(badge_failed_short) : TR(badge_check_short);
		if (ui::section("Diagnostics", ui::icon::kDiagnostics, TR(sec_diagnostics), false, &badge_colour, badge, nullptr,
				ui::Brand::Neutral, false, false, badge_short)) {
			draw_diagnostics_section(state, settings, actions);
			ui::section_end();
		}
	}
}

void draw_osd(const PanelState &state, const Settings &settings)
{
	if (!settings.show_osd)
		return;

	const ui::Theme theme;
	const bool d3d12 = state.d3d12;

	const bool no_upscaler = !settings.enabled ||
		settings.backend == static_cast<unsigned>(BackendChoice::None);
	const ui::Palette &pal = ui::palette();
	ui::osd_begin("##aeon_osd");
	const bool lit = upscaler_lit(state, settings);
	ui::osd_mark();
	ImGui::SameLine(0.0f, ImGui::CalcTextSize(" ").x);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Aeon SR");
	ImGui::SameLine(0.0f, ImGui::CalcTextSize("  ").x);
	if (no_upscaler) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", TR(osd_no_upscaler));
	} else {
		ui::brand_chip(active_brand(state, settings),
			state.optiscaler_upscales ? TR(hd_upscaler_optiscaler)
			: !state.upscaler_label.empty() ? state.upscaler_label.c_str()
			: (state.active_name != nullptr ? state.active_name : "-"), lit);
	}
	const float gap = ImGui::CalcTextSize("  ").x;
	float names = 0.0f;
	if (!no_upscaler)
		names = std::max(names, ImGui::CalcTextSize(TR(osd_upscaler)).x);
	if (settings.neural_render)
		names = std::max(names, ImGui::CalcTextSize(TR(osd_neural)).x);
	const float column = ImGui::GetCursorScreenPos().x + names + gap;
	const auto name = [&](const char *text) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", text);
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetCursorScreenPos(ImVec2(column, ImGui::GetCursorScreenPos().y));
	};
	if (!no_upscaler) {
		name(TR(osd_upscaler));
		ui::pill(status_colour(state.active_status), diag_status_text(state.active_status, i18n::current()), false);
	}
	if (settings.enabled && !state.active_error.empty()) {
		std::string tech, cut;
		const std::string said = diag_player_text(diag_said(narrow_lossy(state.active_error), i18n::current(),
			i18n::S::Count, &tech), &cut);
		if (!said.empty() && cut.empty()) {
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(names + gap + ui::pill_width(TR(st_could_not_start)),
				ImGui::CalcTextSize("0").x * 30.0f));
			ImGui::TextColored(pal.muted, "%s", said.c_str());
			ImGui::PopTextWrapPos();
		}
	}
	if (settings.neural_render) {
		const bool nvidia = vendor_may_be_nvidia(state.gpu_vendor);
		const char *word = !nvidia ? TR(osd_needs_nvidia)
			: state.neural_crashed ? TR(osd_failed)
			: !state.neural_shim_present ? TR(osd_file_missing)
			: state.neural_last == 1 ? TR(osd_running)
			: state.neural_last == -1 ? TR(osd_failed)
			: state.neural_dll_present ? TR(osd_waiting) : TR(osd_file_missing);
		const ImVec4 colour = !nvidia || !state.neural_shim_present ? pal.warn
			: state.neural_crashed || state.neural_last == -1 ? pal.fail
			: state.neural_last == 1 ? pal.ok
			: state.neural_dll_present ? pal.off : pal.warn;
		name(TR(osd_neural));
		ui::pill(colour, word, false);
	}
	ui::osd_end();

	if (settings.debug_info) {
		if (state.active_is_fsr) {
			ImGui::Text("FSR DLL: %s  init: %s  mode: %u",
				state.upscaler_dll_present ? "yes" : "no",
				state.upscaler_initialized ? "yes" : "no",
				settings.upscale_mode);
			ImGui::Text("render: %ux%u  out: %ux%u",
				state.upscaler_render_width, state.upscaler_render_height,
				state.upscaler_out_width, state.upscaler_out_height);
		} else if (state.active_is_xess) {
			ImGui::Text("XeSS DLL: %s  init: %s  mode: %u",
				state.upscaler_dll_present ? "yes" : "no",
				state.upscaler_initialized ? "yes" : "no",
				settings.upscale_mode);
			ImGui::Text("render: %ux%u  out: %ux%u",
				state.upscaler_render_width, state.upscaler_render_height,
				state.upscaler_out_width, state.upscaler_out_height);
		} else if (state.active_is_vsr) {
			ImGui::Text("RTX VSR: capable %s  enh %s  in use %s  lvl %u",
				state.vsr_capable ? "y" : "n", state.vsr_enabled ? "on" : "off",
				state.vsr_in_use ? "y" : "n", state.vsr_level);
			ImGui::Text("render: %ux%u  out: %ux%u",
				state.upscaler_render_width, state.upscaler_render_height,
				state.upscaler_out_width, state.upscaler_out_height);
		} else if (d3d12) {
			ImGui::Text("evals: %llu  mode: %u  preset: %u",
				state.ngx_eval_count, state.ngx_upscale_mode, state.ngx_render_preset);
			ImGui::Text("render: %ux%u  out: %ux%u",
				state.ngx_render_width, state.ngx_render_height, state.ngx_width, state.ngx_height);
		} else {
			ImGui::Text("evals: %llu  mode: %u  preset: %u",
				state.ngx_eval_count, state.ngx_upscale_mode, state.ngx_render_preset);
			ImGui::Text("render: %ux%u  out: %ux%u",
				state.ngx_render_width, state.ngx_render_height, state.ngx_width, state.ngx_height);
		}
		ImGui::Text("scene: %s", scene_state_label(state.scene_state));
		ImGui::Text("flow: %s", state.have_global_flow ? "found" : "missing");
		ImGui::Text("in color: %ux%u %s", state.color_info.width, state.color_info.height, format_label(state.color_info.format));
		ImGui::Text("in MV: %ux%u %s%s  [%s]", state.motion_info.width, state.motion_info.height, format_label(state.motion_info.format),
			state.motion_info.width == 0 ? " (missing!)" : "", motion_provider_label(state.motion_provider));
		ImGui::Text("in depth: %ux%u %s%s  [%s]", state.depth_info.width, state.depth_info.height, format_label(state.depth_info.format),
			state.depth_info.width == 0 ? " (none)" : "", depth_provider_label(state.depth_provider));
		ImGui::Text("reset this frame: %s (cut/clear only)", state.reset_this_frame ? "yes" : "no");
		ImGui::Text("camera motion: %.2f px", state.last_motion_px);
		if (settings.mv_probe && state.probe_valid) {
			ImGui::Text("MV probe: %.0f%% non-zero  mean %.2f px  max %.2f px",
				state.probe_nonzero_pct, state.probe_mean_px, state.probe_max_px);
		}
		ImGui::Text("bias mask: %s", state.bias_mask_bound ? "bound" : "none");
		if (settings.debug_view != 0)
			ImGui::Text("debug view: %s (stage %d)",
				state.debug_view_stage == 1 ? "drawn" : state.debug_view_stage < 0 ? "FAILED" : "idle", state.debug_view_stage);
		if (settings.neural_render) {
			ImGui::Text("neural: evals: %llu  style: %u  intensity: %.2f  detail: %.2f%s",
				static_cast<unsigned long long>(state.neural_eval_count),
				clamp_neural_style(settings.neural_style),
				clamp_neural_unit(settings.neural_intensity, kNeuralIntensityDefault, kNeuralUnitMax),
				clamp_neural_unit(settings.neural_detail_strength, 1.0f, kNeuralDetailMax));
			if (settings.neural_model_scale != 0u && state.neural_model_width != 0)
				ImGui::Text("neural: model at %ux%u", state.neural_model_width, state.neural_model_height);
			if (settings.neural_debug_view != 0u)
				ImGui::Text("neural: debug view %u (frame replaced)", settings.neural_debug_view);
			if (state.neural_gpu_ms > 0.0f) {
				if (state.neural_eval_rows != 0 && state.neural_eval_rows < state.neural_model_height)
					ImGui::Text("neural: %.2f ms  (up to %.0f%% of the model per frame)", state.neural_gpu_ms,
						100.0f * static_cast<float>(state.neural_eval_rows) /
							static_cast<float>(state.neural_model_height));
				else
					ImGui::Text("neural: %.2f ms", state.neural_gpu_ms);
				if (state.frame_time_ms > 0.1f) {
					ImGui::Text("frame:  %.2f ms  (%.0f fps)", state.frame_time_ms,
						1000.0 / static_cast<double>(state.frame_time_ms));
					hint("Presentation to presentation, the last frame the game showed.\n"
						"Not latency: nothing here can see when a frame reaches the screen.");
				}
			}

			{
				const unsigned int want = clamp_neural_passes(settings.neural_passes);
				const unsigned int built = state.neural_passes_built != 0
					? state.neural_passes_built : want;
				if (built > 1u || built < want)
					ImGui::Text("neural: %u chained passes%s", built,
						built < want ? " (fewer than asked for)" : "");
			}
			if (!state.neural_error.empty())
				ImGui::TextWrapped("neural: %s", narrow_lossy(state.neural_error).c_str());
		}
	}
}

}
}

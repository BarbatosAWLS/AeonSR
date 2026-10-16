#include "aeon_sr/core/panel.hpp"
#include "aeon_sr/core/diag_report.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/core/panel_widgets.hpp"
#include "aeon_sr/ngx/ngx_dlssnr.hpp"

#include <dxgiformat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace aeon_sr {
namespace panel {
namespace {

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

void note(const char *fmt, ...) IM_FMTARGS(1);
void note(const char *fmt, ...)
{
	char text[1024];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	ui::wrapped(ui::palette().muted, text);
}

void line(const char *fmt, ...) IM_FMTARGS(1);
void line(const char *fmt, ...)
{
	char text[1024];
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
	if (ImGui::CalcTextSize("Status:").x + gap + ui::pill_width(word) <= ui::avail_width()) {
		ImGui::TextDisabled("Status:");
		ImGui::SameLine(0.0f, gap);
	}
	ui::pill(colour, word);
	if (!see_diagnostics)
		return;
	const char *pointer = "See Diagnostics";
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
	ui::subsection("Image");
	{
		const ui::Dim dim(idle);
		ui::slider_float("Sharpness", &settings.sharpness, 0.0f, 1.0f, "%.2f");
		hint("How sharp the final image looks.");
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		if (state.active_is_dlss) {
			constexpr ui::Brand nv = ui::Brand::Nvidia;
			static const RowOption kPresetOptions[] = {
				{ "Default", "Lets the driver choose", nv },
				{ "E", "Older model", nv },
				{ "F", "Older model", nv },
				{ "J", "Less trailing, a little more shimmer", nv },
				{ "K", "Recommended", nv },
				{ "L", "Suits Ultra Performance", nv },
				{ "M", "Suits Performance", nv },
			};
			if (button_row_value("DLSS preset", &settings.render_preset, kPresetOptions, kFunctionalRenderPresets,
					"Which DLSS image model to use.", !idle)) {
				actions.reset = true;
				actions.persist = true;
			}
		}
	}

	ui::subsection("Reconstruction");
	{
		const ui::Dim dim(idle);
		if (ui::toggle("Sub-pixel jitter", &settings.spatial_jitter)) {
			actions.reset = true;
			actions.invalidate_history = true;
			actions.persist = true;
		}
		hint("Improves edge quality. Leave it on unless something looks wrong.");
	}

	if (settings.spatial_jitter && state.jitter_refused)
		note("Not applied - the upscaler is running without it.");
	const ui::Dim dim(idle && !(settings.neural_render && state.neural_last == 1));
	if (ui::toggle("Upscale the ReShade effects", &settings.upscale_effects)) {
		actions.reset = true;
		actions.invalidate_history = true;
		actions.persist = true;
	}
	hint("Also upscales the other ReShade effects.\n"
		"Turn it off if your sharpening or grain looks washed out.");
	if (ui::button(ui::icon::kUndo, "Reset temporal history"))
		actions.reset = true;
	hint("Clears what the upscaler remembers from previous frames.\n"
		"Try this after a glitch.");
}

void draw_neural_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	ui::band_begin("##neural_band");
	if (ui::toggle("Enable neural rendering", &settings.neural_render)) {
		if (!settings.neural_render)
			actions.neural_restart = true;
		actions.persist = true;
	}
	hint("Adds fine detail to the finished image with NVIDIA DLSS 5.\n"
		"Works with any upscaler, or on its own. Needs an extra runtime file.");

	const bool nvidia = vendor_may_be_nvidia(state.gpu_vendor);
	const bool gpu_refused = nvidia &&
		(state.neural_adapter_unsupported || state.neural_feature_unsupported ||
			!state.neural_runtime_serves_card ||
			state.neural_status == UpscalerStatus::UnsupportedGpu);
	const ui::Palette &pal = ui::palette();
	const bool running = settings.neural_render && state.neural_last == 1;
	const ImVec4 verdict = settings.neural_render ? neural_verdict_colour(state, settings) : pal.off;
	const bool failed = verdict.x == pal.fail.x && verdict.y == pal.fail.y && verdict.z == pal.fail.z;
	const char *word = running ? "Running"
		: !failed ? "Not running"
		: state.neural_driver_too_old || !state.neural_initialized ? "Could not start" : "Failed";
	if (!nvidia || gpu_refused)
		status_line(verdict, "Not available on this GPU", true);
	else if (settings.neural_render)
		status_line(verdict, word, !running);
	ui::band_end(!settings.neural_render);
	if (!settings.neural_render)
		return;
	const bool faded = !nvidia || gpu_refused || (!running && failed);

	ui::subsection("Performance");
	{
		const ui::Dim dim(faded);
		static const RowOption kScaleOptions[] = {
			{ "Full", "Full resolution: most detail, heaviest" },
			{ "90%", "" },
			{ "80%", "" },
			{ "70%", "" },
			{ "60%", "Good balance" },
			{ "50%", "" },
			{ "40%", "Fastest, softest" },
		};
		if (button_row_index<kNeuralModelScaleCount>("Resolution", &settings.neural_model_scale,
				kScaleOptions,
				"The resolution neural rendering works at; the game is not changed.\n"
				"Lower runs faster with softer detail.", false, running))
			actions.persist = true;

		constexpr ui::Brand nv = ui::Brand::Nvidia;
		static const RowOption kModeOptions[] = {
			{ "Quality", "Full detail everywhere", nv },
			{ "Balanced", "Faster; full detail in the middle", nv },
			{ "Performance", "Faster still; less detail towards the edges", nv },
			{ "Ultra Performance", "Fastest; least detail at the edges", nv },
		};
		if (button_row_index<kNeuralModeCount>("DLSS 5 mode", &settings.neural_mode, kModeOptions,
				"Every mode updates the whole picture every frame.", false, running))
			actions.persist = true;

		if (ui::toggle("Run without depth", &settings.neural_no_depth)) {
			actions.neural_restart = true;
			actions.persist = true;
		}
		hint("Skips the depth buffer. Slightly faster.");

		const unsigned int cost_mode = clamp_neural_mode(settings.neural_mode);
		const float rows = cost_mode == 0u ? 1.0f
			: (state.neural_area_share > 0.0f ? state.neural_area_share
				: kNeuralModeKeep[cost_mode] * kNeuralModeKeep[cost_mode]);
		const float rel = neural_chain_relative_cost(
			neural_model_scale_factor(settings.neural_model_scale),
			settings.neural_passes, rows);
		if (running) {
			if (state.neural_gpu_ms > 0.0f) {
				char ms[32];
				std::snprintf(ms, sizeof(ms), "%.2f ms", state.neural_gpu_ms);
				const ui::Span cost[] = { { "Neural pass: ", pal.muted }, { ms, pal.text },
					{ " of GPU time per frame", pal.muted } };
				ui::flow(cost, IM_ARRAYSIZE(cost));
				hint("A 60 fps frame has 16.7 ms in total, and the game needs most of it.");
			}
			note("About %.2fx the cost at Full, Quality and 1 pass", rel);
			hint("Against the same pass at Full resolution, Quality mode and one pass.");
		}
	}

	ui::subsection("Look");
	{
		const ui::Dim dim(faded);
		static const RowOption kStyleOptions[] = {
			{ "Standard", "Neutral" },
			{ "Natural", "Brighter, livelier - can look shiny" },
			{ "Cinematic", "Softer light" },
		};
		if (button_row_index<kNeuralStyleCount>("Style", &settings.neural_style, kStyleOptions,
				"Overall look of the effect.", false, running))
			actions.persist = true;
		ui::slider_float("Detail strength", &settings.neural_detail_strength,
			0.0f, kNeuralDetailMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		hint("How much of the added detail reaches the picture. This is the main slider.");
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		ui::slider_float("Colour strength", &settings.neural_colour_strength,
			0.0f, kNeuralUnitMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		hint("How much of the effect's colour is used.\n"
			"0 keeps the game's own colours and changes only brightness.");
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		ui::slider_float("Neural intensity", &settings.neural_intensity, 0.0f, kNeuralUnitMax, "%.2f",
			ImGuiSliderFlags_AlwaysClamp);
		hint("Strength of the effect itself. It can only reduce it -\n"
			"use Detail strength to push further.");
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		ui::slider_float("Local structure", &settings.neural_local_structure, 0.0f, kNeuralExtendedMax, "%.2f",
			ImGuiSliderFlags_AlwaysClamp);
		hint("How much fine texture neural rendering creates. 1.00 is the default.");
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		ui::slider_float("Local tone", &settings.neural_local_tone, 0.0f, kNeuralExtendedMax, "%.2f",
			ImGuiSliderFlags_AlwaysClamp);
		hint("Local contrast. Lower it if the lighting reacts too strongly.");
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		ui::slider_float("Skin structure", &settings.neural_skin_structure,
			kNeuralSkinMin, kNeuralSkinMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		hint("Detail on skin, set separately from the rest of the frame.\n"
			"-1.00 is the default.");
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			settings.neural_skin_structure = clamp_neural_skin(settings.neural_skin_structure);
			actions.persist = true;
		}
		if (ui::toggle("Auto skin mask", &settings.neural_auto_mask))
			actions.persist = true;
		hint("Finds skin automatically for the slider above.");
	}

	ui::subsection("Stability");
	{
		const ui::Dim dim(faded);
		if (ui::toggle("Smoothing", &settings.neural_smoothing))
			actions.persist = true;
		hint("Steadies the effect between frames and removes trails behind\n"
			"moving objects. Off is sharper but restless.");
		if (ui::toggle("Model without motion vectors", &settings.neural_no_motion_vectors)) {
			actions.neural_restart = true;
			actions.persist = true;
		}
		hint("Makes the effect judge each frame on its own.\n"
			"Removes smearing around moving objects, and costs performance.");
		if (ui::toggle("Reset on camera cut", &settings.neural_reset_on_cut))
			actions.persist = true;
		hint("Clears the effect's history on scene changes.\n"
			"Off by default - it can make the lighting jump.");
	}

	ui::subsection("HDR and highlights");
	{
		const ui::Dim dim(faded);
		ui::slider_float("Paper white", &settings.neural_paper_white,
			kNeuralPaperWhiteMin, kNeuralPaperWhiteMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		hint("Raise it if highlights look blown out in HDR.\n"
			"1.00 changes nothing.");
		if (ImGui::IsItemDeactivatedAfterEdit())
			actions.persist = true;
		if (ui::toggle("Highlight guard", &settings.neural_highlight_guard_on))
			actions.persist = true;
		hint("Limits how much brighter the effect may make a pixel.\n"
			"Turn it on if bright edges glow; it costs some detail in shadows.");
	}

	ui::subsection("Tools");
	{
		const ui::Dim dim(faded);
		static const RowOption kDebugOptions[] = {
			{ "Off", nullptr },
			{ "Input", "What the effect is given" },
			{ "Output", "What the effect produces" },
			{ "Difference", "What it is changing, exaggerated" },
		};
		button_row_index<kNeuralDebugViewCount>("Neural debug view", &settings.neural_debug_view, kDebugOptions,
			"Shows one of the effect's internal images instead of the game.\n"
			"Not saved between sessions.", false, running);

		static const RowOption kKeyOptions[] = {
			{ "None", nullptr }, { "F5", nullptr }, { "F6", nullptr }, { "F7", nullptr }, { "F8", nullptr },
		};
		static const unsigned int kKeyCodes[] = { 0u, 0x74u, 0x75u, 0x76u, 0x77u };
		if (button_row_value("Neural on/off key", &settings.neural_toggle_key, kKeyOptions, kKeyCodes,
				"Key that turns neural rendering on and off while playing.", running))
			actions.persist = true;

		if (ui::button(ui::icon::kRefresh, "Restart neural pass")) {
			actions.neural_restart = true;
			actions.persist = true;
		}
		hint("Reloads the effect. Try this if it stops working.");
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

	ui::subsection("Advanced");
	{
		const ui::Dim dim(faded);
		static const RowOption kPassOptions[] = { { "1x", nullptr }, { "2x", nullptr }, { "3x", nullptr } };
		static const unsigned int kPassValues[] = { 1u, 2u, 3u };
		static_assert(IM_ARRAYSIZE(kPassValues) == static_cast<int>(kNeuralPassMax),
			"the row must offer every pass count up to kNeuralPassMax");
		if (button_row_value("Neural passes##chain", &settings.neural_passes, kPassOptions, kPassValues,
				"Runs neural rendering more than once for stronger detail.\n"
				"Each extra pass costs as much as the first.", running))
			actions.persist = true;

		if (clamp_neural_passes(settings.neural_passes) > 1u &&
			settings.neural_model_scale > 1u)
			ui::callout(pal.warn, ui::icon::kWarn,
				"Extra passes are wasted below 80% Resolution.\n"
				"Raise Resolution, or set Neural passes back to 1x.");
	}
}

#if AEONSR_FRAME_GENERATION
void draw_framegen_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	if (ui::toggle("Frame generation", &settings.frame_generation))
		actions.persist = true;
	hint("Adds frames made from the game's own for smoother motion.\n"
		"Costs a little input delay; moving edges can shimmer slightly.");

	const FrameGenReadout &f = state.framegen;
	const bool running = settings.frame_generation && f.generating;
	static const RowOption kGeneratorOptions[] = {
		{ "AeonSR", "Runs on any graphics card, at any multiplier", ui::Brand::Neutral },
		{ "FSR", "AMD's frame generation. Runs on any graphics card, up to 2x", ui::Brand::Amd },
		{ "XeSS", "Intel's frame generation. Runs on any graphics card, up to 2x", ui::Brand::Intel },
		{ "DLSS", "NVIDIA's frame generation. GeForce RTX 40 up to 2x, RTX 50 up to 6x", ui::Brand::Nvidia },
	};
	if (button_row_index<kFrameGeneratorCount>("Generator", &settings.frame_generator, kGeneratorOptions,
			"Which frame generation makes the frames.\n"
			"If the one chosen cannot run, AeonSR's is used and the panel says why.", false, running))
		actions.persist = true;

	static const RowOption kMultiplyOptions[] = {
		{ "Auto", "As many frames as the screen's refresh has room for, none when the game is as fast" },
		{ "2x", nullptr }, { "3x", nullptr }, { "4x", nullptr }, { "5x", nullptr }, { "6x", nullptr },
	};
	static const unsigned int kMultiplyValues[] = { 0u, 2u, 3u, 4u, 5u, 6u };
	static_assert(IM_ARRAYSIZE(kMultiplyValues) == static_cast<int>(kFrameGenerationMultiplierMax),
		"the row must offer Auto and every multiplier up to kFrameGenerationMultiplierMax");
	if (button_row_value("Multiply by", &settings.frame_generation_multiplier, kMultiplyOptions, kMultiplyValues,
			"Frames shown for every real one. The screen still shows at most its refresh rate.",
			running && !f.through_game_present))
		actions.persist = true;
	if (running && f.through_game_present)
		note("2x here: the frames go through the game's own present");
	if (running && f.own_window && f.own_asked >= 2 && f.own_steps_max >= 1 && f.own_asked > f.own_steps_max)
		note("%s goes up to %ux here", frame_generator_label(f.generator), f.own_steps_max);

	int limit = static_cast<int>(settings.frame_generation_limit);
	if (ui::slider_int("Frame rate limit", &limit, 0, 240, limit == 0 ? "Off" : "%d fps")) {
		settings.frame_generation_limit = static_cast<unsigned int>(std::clamp(limit, 0, 240));
	}
	if (ImGui::IsItemDeactivatedAfterEdit())
		actions.persist = true;
	if (running && f.own_window && f.even_cap_fps != 0) {
		char cap[160];
		std::snprintf(cap, sizeof cap, "Caps the game's real frames.\nAt %u fps motion is even on this %.0f Hz screen.",
			f.even_cap_fps, f.screen_hz);
		hint(cap);
	} else {
		hint("Caps the game's real frames. A cap that divides the screen's refresh\n"
			"(30 or 20 fps on 60 Hz, 25 on 75 Hz) gives the most even motion.");
	}

	if (settings.frame_generation) {
		if (!f.status.empty())
			line("%s", narrow_lossy(f.status).c_str());
		if (f.real_fps > 0.0) {
			if (f.own_window && f.added_latency_ms > 0.0)
				line("Real %.0f fps, shown %.0f fps, +%.0f ms delay", f.real_fps, f.shown_fps, f.added_latency_ms);
			else
				line("Real %.0f fps, shown %.0f fps", f.real_fps, f.shown_fps);
			std::string more;
			char b[256];
			if (f.generating && f.own_window && f.own_steps == 1) {
				more += "Nothing made: the game is as fast as the screen.";
			} else if (f.generating) {
				std::snprintf(b, sizeof b, "Made by %s%s%s", frame_generator_label(f.generator),
					f.generator_version.empty() ? "" : " ", narrow_lossy(f.generator_version).c_str());
				more += b;
				if (f.own_window && f.own_steps >= 2)
					std::snprintf(b, sizeof b, " at %ux%s.", f.own_steps, f.own_auto ? " (Auto)" : "");
				else if (f.own_window)
					std::snprintf(b, sizeof b, " at every refresh (Auto).");
				else
					std::snprintf(b, sizeof b, ".");
				more += b;
			}
			if (f.own_window && f.screen_latency_ms > 0.0 && f.added_latency_ms > 0.0) {
				std::snprintf(b, sizeof b, "\nA real frame reaches the screen %.0f ms after the game presents it;\n"
					"%.0f ms of that is frame generation.", f.screen_latency_ms, f.added_latency_ms);
				more += b;
			}
			if (f.own_window)
				more += "\nReShade's own FPS counter and screenshots see the real frames only.";
			if (!f.route.empty())
				more += "\nPresented with " + f.route + ".";
			if (!more.empty())
				hint(more.c_str());
		}
	}

	ui::subsection("Pacing and latency");
	if (ui::toggle("Even frame pacing", &settings.pace_to_display))
		actions.persist = true;
	hint("When the game runs a little faster than the screen (up to 2.5x its refresh),\n"
		"shows one frame per refresh so motion is even. Frame generation paces its own frames.");

	static const RowOption kLatencyOptions[] = {
		{ "Off", "Frames start when the game starts them", ui::Brand::Neutral },
		{ "AeonSR", "AeonSR's own: the frame waits before the game reads your input, not after.\n"
			"Any graphics card", ui::Brand::Neutral },
		{ "Reflex", "NVIDIA Reflex, on every graphics API. Needs a GeForce card;\n"
			"elsewhere AeonSR's own is used", ui::Brand::Nvidia },
	};
	if (button_row_index<3>("Low latency", &settings.low_latency, kLatencyOptions,
			"Less delay between your input and the screen. Frame generation, when on, paces its own frames."))
		actions.persist = true;
}
#endif

void draw_experiments_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	ui::subsection("Upscaler input");
	static const RowOption kUpscaleOptions[] = {
		{ "Native", "Full resolution - anti-aliasing only" },
		{ "Ultra Quality", "Not offered by every upscaler; falls back to Quality" },
		{ "Quality" },
		{ "Balanced" },
		{ "Performance" },
		{ "Ultra Performance" },
	};
	constexpr unsigned int kUpscaleModeCount = static_cast<unsigned int>(UpscaleMode::UltraPerformance) + 1u;
	{
		const ui::Dim dim(upscaler_off(settings));
		if (button_row_index<kUpscaleModeCount>("Mode", &settings.upscale_mode, kUpscaleOptions,
				"The resolution the upscaler works from. The game still renders at full\n"
				"resolution: lower only makes the upscaler's own work cheaper, and loses detail.",
				false, true, true)) {
			actions.reset = true;
			actions.invalidate_history = true;
			actions.persist = true;
		}
	}
	if (upscaler_off(settings))
		note("Needs an upscaler on (Temporal Anti-Aliasing).");
	const GameScaleVerdict verdict = state.game_scale_verdict;
	const bool wrong_api = verdict == GameScaleVerdict::WrongApi;
	{
		const ui::Dim dim(upscaler_off(settings) || wrong_api);
		bool scale_on = settings.game_render_scale;
		if (ui::toggle("Use the game's render scale", &scale_on) && (!wrong_api || !scale_on)) {
			settings.game_render_scale = scale_on;
			actions.reset = true;
			actions.invalidate_history = true;
			actions.persist = true;
		}
		if (state.game_integrated)
			hint("The game really draws a smaller picture, at the size Mode above asks for:\n"
				"Aeon SR sets the game's own image quality, and the upscaler brings the picture\n"
				"back to full size. The game's interface stays at full resolution.\n"
				"Off, the game draws at its own setting.");
		else
			hint("For games with their own render scale (image quality, resolution scale):\n"
				"lower it in the game and the game really draws a smaller picture, so it runs\n"
				"faster, and the upscaler brings it back to full size. The game's interface\n"
				"stays at full resolution. Mode above has no effect while this is in use.");
	}
	if (state.game_integrated) {
		if (!settings.game_render_scale)
			note("%s is supported: turn this on and Mode above sets the game's own image quality.",
				state.game_title.c_str());
		else if (upscaler_off(settings))
			note("%s: with no upscaler on, the game draws at its own setting.", state.game_title.c_str());
		else if (state.game_scale_render_w == 0 && !state.game_note.empty())
			note("%s: %s.", state.game_title.c_str(), narrow_lossy(state.game_note).c_str());
		else
			note("%s: the game itself draws at the size Mode asks for.", state.game_title.c_str());
		if (settings.game_render_scale && state.game_scale_render_w != 0)
			note("Drawn at %ux%u, brought to %ux%u by the upscaler.", state.game_scale_render_w,
				state.game_scale_render_h, state.game_scale_w, state.game_scale_h);
	} else {
		if (!state.game_title.empty())
			note("%s: its render scale is left to the game (%s).", state.game_title.c_str(),
				narrow_lossy(state.game_note).c_str());
		switch (verdict) {
		case GameScaleVerdict::WrongApi:
			note("Not supported in this game: it runs on %s, and this works only in Direct3D 10 and 11 games.",
				state.api_name != nullptr ? state.api_name : "another graphics API");
			break;
		case GameScaleVerdict::Working:
		case GameScaleVerdict::Idle:
			if (state.game_scale_render_w != 0 && state.game_scale_render_w < state.game_scale_w)
				note("Working: the game draws at %ux%u; the upscaler brings it to %ux%u.", state.game_scale_render_w,
					state.game_scale_render_h, state.game_scale_w, state.game_scale_h);
			else if (state.game_scale_render_w != 0)
				note("Working, at the game's 100%%: lower its own render scale to make it draw less.");
			else
				note("Supported in this game; not in use on this screen (menus and loading screens).");
			break;
		case GameScaleVerdict::Looking:
			note("Looking for the game's render scale. Play for a moment with it below 100%%.");
			break;
		case GameScaleVerdict::OtherCorner:
			note("Not supported in this game: it shrinks a picture, but not the one it draws the scene in.");
			break;
		case GameScaleVerdict::NotFound:
			note("Not supported in this game: in 30 s of play it never drew a smaller picture to stretch. "
				"(At 100%% of its own render scale a game draws none: lower it first.)");
			break;
		case GameScaleVerdict::Off:
			break;
		}
	}

	ui::subsection("Neural screenshot (DLSS 5)");
	{
		static const RowOption kAccumOptions[] = {
			{ "2x", nullptr }, { "4x", nullptr }, { "8x", "A good place to start" },
			{ "16x", nullptr }, { "32x", nullptr }, { "64x", "Slowest, and it can look overcooked" },
		};
		static const unsigned int kAccumValues[] = { 2u, 4u, 8u, 16u, 32u, 64u };
		const bool neural_running = settings.neural_render && state.neural_last == 1;
		if (button_row_value("Screenshot passes##screenshot", &settings.accum_iterations,
				kAccumOptions, kAccumValues,
				"How many neural rendering passes to stack on the held picture.\n"
				"More adds detail and takes longer.", neural_running))
			actions.persist = true;

		const bool can_run = neural_running && !state.upscalers_remote;
		ImGui::BeginDisabled(!can_run && !state.accum_running);
		if (!state.accum_running) {
			if (ui::button(ui::icon::kPlay, "Freeze and add neural detail"))
				actions.accum_start = true;
		} else if (ui::button(ui::icon::kStop, "Release picture")) {
			actions.accum_stop = true;
		}
		ImGui::EndDisabled();
		hint("Holds the picture and runs neural rendering over it again and again.\n"
			"Release picture returns to the game.");
		if (state.upscalers_remote)
			note("Not available in this game: the neural pass runs in a helper process.");

		const ui::Palette &pal = ui::palette();
		if (state.accum_running && !state.accum_holding) {
			char progress[64];
			std::snprintf(progress, sizeof(progress), "Adding neural detail: pass %u of %u",
				state.accum_done, state.accum_target);
			ImGui::ProgressBar(state.accum_target != 0
					? static_cast<float>(state.accum_done) / static_cast<float>(state.accum_target) : 0.0f,
				ImVec2(ui::avail_width(), 0.0f), progress);
		} else if (state.accum_holding) {
			ui::calloutf(pal.ok, ui::icon::kOk,
				"Held after %u neural passes - take the screenshot, then Release picture.", state.accum_done);
		} else if (!can_run && !state.upscalers_remote) {
			note("Needs neural rendering to be on and running.");
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
	const std::string tip = "Technical: " + tech + "\nThe saved report has it too.";
	hint(tip.c_str());
}

void draw_health(const PanelState &state, const Settings &settings, bool skip_log)
{
	std::vector<DiagCheck> checks = diag_checks(state, settings);
	if (skip_log)
		checks.erase(std::remove_if(checks.begin(), checks.end(),
			[](const DiagCheck &c) { return std::string(c.name) == "Log"; }), checks.end());
	float name_w = 0.0f;
	for (const DiagCheck &c : checks)
		name_w = std::max(name_w, ImGui::CalcTextSize(c.name).x);
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
			hint(check_state_label(c.state));
		} else {
			ImGui::TextColored(check_colour(c.state), "[%s]", check_state_label(c.state));
		}
		ImGui::TableNextColumn();
		if (beside) {
			ImGui::TextUnformatted(c.name);
			ImGui::TableNextColumn();
		}
		if (!beside)
			ImGui::TextColored(ui::palette().label, "%s", c.name);
		std::string tech;
		line("%s", diag_player_text(c.detail, &tech).c_str());
		technical_hint(tech);
		if (!c.action.empty())
			note("%s", c.action.c_str());
	}
	ImGui::EndTable();
	ui::tray_end();
}

void draw_report_tools(const PanelState &state, Settings &settings, PanelActions &actions)
{
	if (ui::button(ui::icon::kSave, "Save report"))
		actions.save_report = true;
	hint("Writes a .zip beside the add-on with this report, the logs, the settings,\n"
		"the last seconds of frames and a picture of this frame before and after\n"
		"Aeon SR. Attach it when reporting a problem.");
	ui::same_line_if_fits(ui::button_width(ui::icon::kCopy, "Copy report"));
	if (ui::button(ui::icon::kCopy, "Copy report"))
		actions.copy_report = true;
	hint("The same report, straight to the clipboard.");

	if (!state.report_error.empty())
		ui::callout(ui::palette().fail, ui::icon::kAlert, narrow_lossy(state.report_error).c_str());
	else if (!state.report_path.empty())
		note("Report: %s", narrow_lossy(state.report_path).c_str());

	if (ui::toggle("Detailed log", &settings.verbose_log))
		actions.persist = true;
	hint("Records every step of every frame into AeonSR.log.\n"
		"Turn it on only while reproducing a problem - it costs performance.");
	if (!state.log_path.empty())
		note("Log: %s", narrow_lossy(state.log_path).c_str());
}

std::string problem_source(const std::string &tag)
{
	std::string key;
	for (char c : tag)
		if (std::isalnum(static_cast<unsigned char>(c)))
			key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	static const struct { const char *key, *name; } kNames[] = {
		{ "neural", "Neural rendering" }, { "neuralrender", "Neural rendering" }, { "dlss", "DLSS" },
		{ "vsr", "RTX VSR" }, { "d3d12", "Direct3D 12" }, { "hud", "HUD" }, { "aeonsr", "Aeon SR" },
		{ "host", "Helper process" }, { "remote", "Helper process" }, { "debugview", "Motion and depth view" },
		{ "flow", "Motion vectors" }, { "probe", "Motion probe" }, { "interop", "Bridge" }, { "shader", "Shaders" },
	};
	for (const auto &n : kNames)
		if (key == n.key)
			return n.name;
	std::string out;
	for (size_t i = 0; i < tag.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(tag[i]);
		if (c == '-' || c == '_') {
			out.push_back(' ');
		} else if (i > 0 && std::isupper(c) && std::islower(static_cast<unsigned char>(tag[i - 1]))) {
			out.push_back(' ');
			out.push_back(static_cast<char>(std::tolower(c)));
		} else {
			out.push_back(static_cast<char>(c));
		}
	}
	if (!out.empty())
		out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
	return out;
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
	for (const DiagCheck &c : diag_checks(state, settings))
		if (std::string(c.name) == "Log")
			return c.detail;
	return std::string();
}

void draw_recent_problems(const std::vector<DiagEntry> &problems, const std::string &count)
{
	if (problems.empty())
		return;
	ui::subsection("Recent problems", count.c_str());
	const ui::Palette &pal = ui::palette();
	const char *warn_icon = ui::glyph(ui::icon::kWarn);
	const char *fail_icon = ui::glyph(ui::icon::kFail);
	const float gap = ImGui::CalcTextSize(" ").x;
	const float icon_w = std::max(ImGui::CalcTextSize(warn_icon).x, ImGui::CalcTextSize(fail_icon).x);
	float tag_w = 0.0f;
	for (const DiagEntry &e : problems)
		tag_w = std::max(tag_w, ImGui::CalcTextSize(problem_source(e.tag).c_str()).x);
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
		ImGui::TextUnformatted(problem_source(e.tag).c_str());
	};
	for (auto it = problems.rbegin(); it != problems.rend(); ++it) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextDisabled("%.1fs", static_cast<double>(it->time_s));
		hint("Seconds since the game started.");
		ImGui::TableNextColumn();
		if (beside) {
			source(*it);
			ImGui::TableNextColumn();
		}
		if (!beside)
			source(*it);
		std::string tech;
		line("%s", diag_player_text(narrow_lossy(it->text), &tech).c_str());
		technical_hint(tech);
	}
	ImGui::EndTable();
	ui::tray_end();
}

void draw_neural_details(const PanelState &state, const Settings &settings)
{
	if (!settings.neural_render)
		return;
	ui::subsection("Neural rendering");
	if (!state.upscalers_remote && state.neural_dll_found && !state.neural_runtime_file.empty()) {
		const size_t cut = state.neural_runtime_file.find_last_of(L"\\/");
		const std::string name = narrow_lossy(cut == std::wstring::npos
			? state.neural_runtime_file : state.neural_runtime_file.substr(cut + 1));
		if (state.neural_runtime_serves_card)
			note("Runtime: %s, built for %s", name.c_str(),
				state.neural_runtime_cards.empty() ? "unknown cards" : state.neural_runtime_cards.c_str());
		else
			note("Runtime: %s", name.c_str());
		hint("A build in runtime\\dlss5-allgpu\\ is tried first.");
	}
	if (state.neural_last == 1 && state.neural_model_width != 0) {
		if (state.neural_eval_rows != 0 && state.neural_eval_rows < state.neural_model_height)
			note("Working at %ux%u, up to %.0f%% of it per frame", state.neural_model_width,
				state.neural_model_height, 100.0 * state.neural_eval_rows / state.neural_model_height);
		else
			note("Working at %ux%u", state.neural_model_width, state.neural_model_height);
	}
	if (!state.neural_driver_too_old && !state.neural_adapter_unsupported) {
		const bool arch = state.neural_requirement_known && state.neural_min_architecture != 0;
		const bool driver = state.neural_required_driver_major != 0;
		if (arch && driver)
			note("Runtime needs %s or newer, and driver %u.%02u or newer",
				neural_arch_name(state.neural_min_architecture),
				state.neural_required_driver_major, state.neural_required_driver_minor);
		else if (arch)
			note("Runtime needs %s or newer", neural_arch_name(state.neural_min_architecture));
		else if (driver)
			note("Runtime needs driver %u.%02u or newer",
				state.neural_required_driver_major, state.neural_required_driver_minor);
	}
	note("Frame colour space: %s",
		state.neural_color_space == NeuralColorSpace::Pq ? "HDR10 (PQ)"
		: state.neural_color_space == NeuralColorSpace::ScrgbLinear ? "scRGB (linear)"
		: "SDR / sRGB");
	note("Passes run: %llu", static_cast<unsigned long long>(state.neural_eval_count));
}

void draw_diagnostics_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	const std::vector<DiagEntry> problems = recent_problems(state, settings);
	ui::subsection("Health");
	draw_health(state, settings, !problems.empty());

	if (ui::toggle("Show on-screen status", &settings.show_osd))
		actions.persist = true;
	hint("Shows the upscaler and neural rendering status in a small window over the game.");

	ui::subsection("Report");
	draw_report_tools(state, settings, actions);

	draw_recent_problems(problems, log_count(state, settings));

	if (!ui::disclosure("Details", "Details"))
		return;

	note("Build: %s", state.build_stamp);
	note("Camera motion: %.2f px", state.last_motion_px);
	if (state.jitter_in_frame && state.jitter_still) {
		note("Jitter: on - nothing drawn took it this frame, so the frame is left still");
	} else if (state.jitter_in_frame && state.jitter_mixed) {
		note("Jitter: drawn into the game - mixed this frame, the upscaler was told none");
	} else if (state.jitter_in_frame && state.jitter_aliased_draws != 0) {
		note("Jitter: drawn into the game, %u of %u draws moved, %u with another depth buffer",
			state.jitter_moved_draws, state.jitter_moved_draws + state.jitter_plain_draws,
			state.jitter_aliased_draws);
	} else if (state.jitter_in_frame) {
		note("Jitter: drawn into the game, %u of %u draws moved",
			state.jitter_moved_draws, state.jitter_moved_draws + state.jitter_plain_draws);
	} else if (settings.spatial_jitter && state.jitter_withheld) {
		note(state.jitter_active ? "Jitter: on, not drawn - the game jitters its own frame, resampled as it is scaled down"
			: "Jitter: on, not drawn - the game jitters its own frame");
	} else if (settings.spatial_jitter && !state.jitter_refused && !state.jitter_active) {
		note("Jitter: on, idle this frame");
	} else if (settings.spatial_jitter && !state.jitter_refused && state.jitter_note[0] != '\0') {
		note("Jitter: resampled - %s", state.jitter_note);
	} else if (settings.spatial_jitter && !state.jitter_refused) {
		note("Jitter: on, waiting for the game to draw a frame");
	}
	if (state.scope_capture_enabled && state.scope_have && ImGui::TreeNode("Jitter numbers")) {
		draw_jitter_numbers(state, actions);
		ImGui::TreePop();
	}
	if (ui::toggle("Probe the motion vectors", &settings.mv_probe)) {
		if (!settings.mv_probe)
			actions.probe_reset = true;
		actions.persist = true;
	}
	hint("Checks now and then that motion detection is working; the result is\n"
		"the Motion vectors row in Health.");
	if (settings.mv_probe && state.probe_unsupported)
		ui::callout(ui::palette().fail, ui::icon::kAlert,
			"Probe: format cannot be read back (expected RG16F or RG32F).");

	if (state.active_is_fsr) {
		const std::wstring &running = state.fsr_provider_version;
		const bool running_4 = running.size() > 1 && running[0] == L'4' && running[1] == L'.';
		bool offers_4 = false;
		for (const std::string &p : state.fsr_providers)
			offers_4 = offers_4 || (p.size() > 1 && p[0] == '4' && p[1] == '.');

		if (running.empty())
			note("The runtime did not say which FSR version it used");
		if (offers_4 && !running_4) {
			ui::calloutf(ui::palette().warn, ui::icon::kWarn,
				"This runtime offers FSR 4, and fell back to %s on this GPU.",
				running.empty() ? "an older version" : narrow_lossy(running).c_str());
			hint("FSR 4 runs on RDNA 4, and on RDNA 3 desktop cards with a recent\n"
				"Adrenalin driver. Everything else gets FSR 3.1.");
		}
	} else if (state.active_is_vsr) {
		note("RTX VSR: %s, driver enhancement %s, %s, level %u",
			state.vsr_capable ? "capable" : "not capable",
			state.vsr_enabled ? "on" : "off",
			state.vsr_in_use ? "in use" : "not in use", state.vsr_level);
	}

	draw_neural_details(state, settings);

	ui::subsection("Motion and depth view");
	static const RowOption kDebugViewOptions[] = {
		{ "Off" },
		{ "Flow", "Motion as colour: hue is direction, brightness is speed" },
		{ "Arrows", "Motion as arrows over the game - the readable one" },
		{ "Confidence", "How sure the motion is: red unsure, green certain" },
		{ "Depth", "The depth the upscalers are given" },
	};
	if (button_row_index<kDebugViewCount>("##Motion and depth view", &settings.debug_view, kDebugViewOptions,
			"Shows the motion or depth Aeon SR reads, instead of the game.\n"
			"Nothing else changes."))
		actions.persist = true;
	note("Drawn on screen, over the game.");
	if (settings.debug_view != 0) {
		const char *stage = "starting...";
		switch (state.debug_view_stage) {
		case 1: stage = "drawing"; break;
		case -1: stage = "FAILED: invalid arguments"; break;
		case -2: stage = "FAILED: the shader would not compile"; break;
		case -3: stage = "FAILED: unsupported source format"; break;
		case -4: stage = "FAILED: no render target"; break;
		case -5: stage = "FAILED: no graphics context"; break;
		case -6: stage = "waiting"; break;
		case -7: stage = "FAILED: the draw was rejected"; break;
		case -8: stage = "FAILED: frame copy view"; break;
		case -9: stage = "FAILED: out of memory for the copies"; break;
		default: break;
		}
		const bool ok = state.debug_view_stage == 1;
		const bool waiting = state.debug_view_stage == -6;
		const ui::Palette &pal = ui::palette();
		status_line(ok ? pal.ok : waiting ? pal.warn : pal.fail, stage, false);
		if (state.debug_view_note != nullptr && state.debug_view_note[0] != 0)
			note("%s", state.debug_view_note);
	}

	ui::subsection("About");
	note("Powered by NVIDIA DLSS and the NVIDIA NGX SDK.");
	note("NVIDIA, NVIDIA RTX and GeForce RTX are trademarks of NVIDIA Corporation.");
	note("AMD FidelityFX Super Resolution (c) Advanced Micro Devices, Inc. - MIT.");
	note("Intel XeSS (c) Intel Corporation - Intel Simplified Software License.");
	note("Full terms in THIRD-PARTY-NOTICES.md beside the add-on.");
	ui::disclosure_end();
}

const RowOption kBackendOptions[] = {
	{ "DLSS", "NVIDIA's. Needs a GeForce RTX", ui::Brand::Nvidia },
	{ "FSR", "AMD's. Runs on any graphics card", ui::Brand::Amd },
	{ "XeSS", "Intel's. Runs on any graphics card", ui::Brand::Intel },
	{ "None", "No upscaler. Neural rendering still runs if it is on.", ui::Brand::None },
	{ "RTX VSR", "NVIDIA only, DirectX 11 and SDR. Needs RTX video enhancement\n"
		"switched on in the NVIDIA Control Panel.", ui::Brand::Nvidia },
};

ui::Brand chosen_brand(const Settings &settings)
{
	if (!settings.enabled || settings.backend >= static_cast<unsigned>(IM_ARRAYSIZE(kBackendOptions)))
		return ui::Brand::Neutral;
	return kBackendOptions[settings.backend].brand;
}

ui::Brand active_brand(const PanelState &state, const Settings &settings)
{
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
	if (state.active_is_fsr)
		return "FSR";
	if (state.active_is_xess)
		return "XeSS";
	if (state.active_is_vsr)
		return "RTX VSR";
	if (state.active_is_dlss)
		return "DLSS";
	return settings.backend < static_cast<unsigned>(IM_ARRAYSIZE(kBackendOptions))
		? kBackendOptions[settings.backend].label : nullptr;
}

}

void draw_taa_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	ui::band_begin("##taa_band");
	bool enabled = settings.enabled;
	if (ui::toggle("Enable upscaler", &enabled)) {
		settings.enabled = enabled;
		actions.reset = true;
		actions.persist = true;
	}
	hint("Turns the upscaler on. Neural rendering has its own switch.");

	if (button_row_index<kBackendChoiceCount>("##upscaler", &settings.backend, kBackendOptions,
			"Which upscaler to use.", true, upscaler_lit(state, settings))) {
		actions.persist = true;
		actions.reset = true;
	}

	if (upscaler_off(settings))
		status_line(ui::palette().off, "off", false);
	else if (!state.upscaler_label.empty() || state.active_name != nullptr)
		status_line(status_colour(state.active_status), status_label(state.active_status),
			!state.active_error.empty());

	if (state.active_is_fsr && state.fsr_providers.size() > 1) {
		std::vector<RowOption> opts;
		opts.push_back(RowOption{ "Auto", "Picks the best version this GPU can run", ui::Brand::Amd });
		for (const std::string &pv : state.fsr_providers)
			opts.push_back(RowOption{ pv.c_str(), nullptr, ui::Brand::Amd });
		int idx = 0;
		for (size_t i = 0; i < state.fsr_providers.size(); ++i) {
			if (state.fsr_providers[i] == settings.fsr_provider)
				idx = static_cast<int>(i) + 1;
		}
		if (button_row_n("FSR version", &idx, opts.data(), static_cast<int>(opts.size()),
				"Locks FSR to one version instead of letting it choose.", false, upscaler_lit(state, settings))) {
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
	if (ui::section("Temporal Anti-Aliasing", ui::icon::kTaa, "Temporal Anti-Aliasing", true, nullptr, nullptr,
			active_short_name(state, settings), active_brand(state, settings), upscaler_running, true)) {
		draw_taa_section(state, settings, actions);
		ui::section_end();
	}
	const bool neural_running = settings.neural_render && state.neural_last == 1;
	if (ui::section("Neural Rendering", ui::icon::kNeural, "Neural Rendering", false, nullptr, nullptr,
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
	if (ui::section("Frame generation (experimental)", ui::icon::kFrames, "Frame generation (experimental)", false,
			nullptr, nullptr, settings.frame_generation ? kFgShort[static_cast<unsigned int>(fg_maker) % kFrameGeneratorCount]
				: nullptr, fg_brand,
			settings.frame_generation && state.framegen.generating, true)) {
		draw_framegen_section(state, settings, actions);
		ui::section_end();
	}
#endif
	if (ui::section("Experiments", ui::icon::kExperiments, "Experiments", false)) {
		draw_experiments_section(state, settings, actions);
		ui::section_end();
	}
	{
		const CheckState worst = diag_worst(diag_checks(state, settings));
		const ImVec4 badge_colour = check_colour(worst);
		const char *badge = worst == CheckState::Fail ? "something failed"
			: worst == CheckState::Warn ? "check this" : nullptr;
		const char *badge_short = worst == CheckState::Fail ? "Failed" : "Check";
		if (ui::section("Diagnostics", ui::icon::kDiagnostics, "Diagnostics", false, &badge_colour, badge, nullptr,
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
		ImGui::TextDisabled("No upscaler");
	} else {
		ui::brand_chip(active_brand(state, settings),
			!state.upscaler_label.empty() ? state.upscaler_label.c_str()
			: (state.active_name != nullptr ? state.active_name : "-"), lit);
	}
	const float gap = ImGui::CalcTextSize("  ").x;
	float names = 0.0f;
	if (!no_upscaler)
		names = std::max(names, ImGui::CalcTextSize("Upscaler").x);
	if (settings.neural_render)
		names = std::max(names, ImGui::CalcTextSize("Neural (DLSS 5)").x);
	const float column = ImGui::GetCursorScreenPos().x + names + gap;
	const auto name = [&](const char *text) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", text);
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetCursorScreenPos(ImVec2(column, ImGui::GetCursorScreenPos().y));
	};
	if (!no_upscaler) {
		name("Upscaler");
		ui::pill(status_colour(state.active_status), status_label(state.active_status), false);
	}
	if (settings.enabled && !state.active_error.empty()) {
		std::string tech;
		const std::string said = diag_player_text(narrow_lossy(state.active_error), &tech);
		if (tech.empty()) {
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(names + gap + ui::pill_width("Could not start"),
				ImGui::CalcTextSize("0").x * 30.0f));
			ImGui::TextColored(pal.muted, "%s", said.c_str());
			ImGui::PopTextWrapPos();
		}
	}
	if (settings.neural_render) {
		const bool nvidia = vendor_may_be_nvidia(state.gpu_vendor);
		const char *word = !nvidia ? "needs NVIDIA"
			: state.neural_crashed ? "failed"
			: !state.neural_shim_present ? "file missing"
			: state.neural_last == 1 ? "running"
			: state.neural_last == -1 ? "failed"
			: state.neural_dll_present ? "waiting" : "file missing";
		const ImVec4 colour = !nvidia || !state.neural_shim_present ? pal.warn
			: state.neural_crashed || state.neural_last == -1 ? pal.fail
			: state.neural_last == 1 ? pal.ok
			: state.neural_dll_present ? pal.off : pal.warn;
		name("Neural (DLSS 5)");
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

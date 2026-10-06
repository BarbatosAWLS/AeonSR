#include "aeon_sr/core/panel.hpp"
#include "aeon_sr/core/diag_report.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/ngx/ngx_dlssnr.hpp"

#include <dxgiformat.h>

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
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", text);
}

struct RowOption {
	const char *label;
	const char *tip = nullptr;
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
	if (!ImGui::BeginTooltip())
		return;
	if (opt_tip != nullptr)
		ImGui::TextUnformatted(opt_tip);
	if (opt_tip != nullptr && row_tip != nullptr)
		ImGui::Separator();
	if (row_tip != nullptr)
		ImGui::TextUnformatted(row_tip);
	ImGui::EndTooltip();
}

bool button_row_n(const char *caption, int *idx, const RowOption *opts, int n, const char *row_tip)
{
	const int N = n;
	const ImGuiStyle &style = ImGui::GetStyle();

	const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;

	const float min_w = ImGui::GetFrameHeight() * 2.0f;

	bool changed = false;
	bool tip_shown = false;
	ImGui::PushID(caption);
	ImGui::BeginGroup();
	for (int i = 0; i < N; ++i) {
		const char *label = opts[i].label;
		float w = ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
		if (w < min_w)
			w = min_w;

		if (i > 0 && ImGui::GetItemRectMax().x + style.ItemSpacing.x + w <= right)
			ImGui::SameLine();

		const bool selected = (i == *idx);
		if (selected) {
			const ImVec4 lit = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
			ImGui::PushStyleColor(ImGuiCol_Button, lit);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, lit);
		}
		ImGui::PushID(i);
		const bool pressed = ImGui::Button(label, ImVec2(w, 0.0f));
		ImGui::PopID();
		if (selected) {
			ImGui::PopStyleColor(2);

			ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
				ImGui::GetColorU32(ImGuiCol_Text, 0.55f), style.FrameRounding, 0, 1.0f);
		}

		if (!tip_shown && ImGui::IsItemHovered(kRowHover)) {
			row_tooltip(opts[i].tip, row_tip);
			tip_shown = true;
		}
		if (pressed && !selected) {
			*idx = i;
			changed = true;
		}
	}

	const char *caption_end = visible_end(caption);
	if (caption_end != caption) {
		const float caption_w = ImGui::CalcTextSize(caption, caption_end).x;
		if (ImGui::GetItemRectMax().x + style.ItemInnerSpacing.x + caption_w <= right)
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		else
			ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(caption, caption_end);
	}
	ImGui::EndGroup();

	if (!tip_shown && ImGui::IsItemHovered(kRowHover))
		row_tooltip(nullptr, row_tip);
	ImGui::PopID();
	return changed;
}

template <int N>
bool button_row(const char *caption, int *idx, const RowOption (&opts)[N], const char *row_tip)
{
	static_assert(N > 0, "a row needs at least one option");
	return button_row_n(caption, idx, opts, N, row_tip);
}

template <unsigned int Count, int N>
bool button_row_index(const char *caption, unsigned int *field, const RowOption (&opts)[N], const char *row_tip)
{
	static_assert(N == static_cast<int>(Count), "one option per value the field can hold");
	int idx = *field < Count ? static_cast<int>(*field) : -1;
	if (!button_row(caption, &idx, opts, row_tip))
		return false;
	*field = static_cast<unsigned int>(idx);
	return true;
}

template <typename T, int N, int M>
bool button_row_value(const char *caption, T *field, const RowOption (&opts)[N], const T (&values)[M], const char *row_tip)
{
	static_assert(N == M, "labels and values must stay in step");
	int idx = -1;
	for (int i = 0; i < M; ++i)
		if (values[i] == *field) { idx = i; break; }
	if (!button_row(caption, &idx, opts, row_tip))
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
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Fault injected: %s", state.scope_fault.c_str());
	if (state.scope_capture_enabled) {
		if (ImGui::Button("Capture jitter scope"))
			actions.scope_capture = true;
		hint("Saves the next frames at every stage, lossless, for tools/jitter_scope.");
		if (!state.scope_status.empty())
			ImGui::TextWrapped("Scope: %s", narrow_lossy(state.scope_status).c_str());
	}
}

void draw_quality_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	ImGui::SeparatorText("Reconstruct");
	if (ImGui::Checkbox("Upscale the ReShade effects", &settings.upscale_effects)) {
		actions.reset = true;
		actions.invalidate_history = true;
		actions.persist = true;
	}
	hint("Also upscales the other ReShade effects.\n"
		"Turn it off if your sharpening or grain looks washed out.");

	if (ImGui::Checkbox("Sub-pixel jitter", &settings.spatial_jitter)) {
		actions.reset = true;
		actions.invalidate_history = true;
		actions.persist = true;
	}
	hint("Improves edge quality. Leave it on unless something looks wrong.");

	if (settings.spatial_jitter && state.jitter_refused) {
		ImGui::TextDisabled("Not applied - the upscaler is running without it.");
	} else if (state.jitter_in_frame && state.jitter_still) {
		ImGui::TextDisabled("On - nothing drawn took it this frame, so the frame is left still.");
	} else if (state.jitter_in_frame && state.jitter_mixed) {
		ImGui::TextDisabled("Drawn into the game - mixed this frame, the upscaler was told none.");
	} else if (state.jitter_in_frame && state.jitter_aliased_draws != 0) {
		ImGui::TextDisabled("Drawn into the game: %u of %u draws moved, %u with another depth buffer.",
			state.jitter_moved_draws, state.jitter_moved_draws + state.jitter_plain_draws,
			state.jitter_aliased_draws);
	} else if (state.jitter_in_frame) {
		ImGui::TextDisabled("Drawn into the game: %u of %u draws moved.",
			state.jitter_moved_draws, state.jitter_moved_draws + state.jitter_plain_draws);
	} else if (settings.spatial_jitter && !state.jitter_active) {
		ImGui::TextDisabled("On, idle this frame.");
	} else if (settings.spatial_jitter && state.jitter_note[0] != '\0') {
		ImGui::TextDisabled("Resampled - %s.", state.jitter_note);
	} else if (settings.spatial_jitter) {
		ImGui::TextDisabled("On, waiting for the game to draw a frame.");
	}
	if ((settings.spatial_jitter || state.scope_capture_enabled) && state.scope_have &&
		ImGui::TreeNode("Jitter numbers")) {
		draw_jitter_numbers(state, actions);
		ImGui::TreePop();
	}

	if (state.active_is_dlss) {
		ImGui::SeparatorText("DLSS");

		static const RowOption kPresetOptions[] = {
			{ "Default", "Lets the driver choose" },
			{ "E", "Older model" },
			{ "F", "Older model" },
			{ "J", "Less trailing, a little more shimmer" },
			{ "K", "Recommended" },
			{ "L", "Suits Ultra Performance" },
			{ "M", "Suits Performance" },
		};
		if (button_row_value("Render preset", &settings.render_preset, kPresetOptions, kFunctionalRenderPresets,
				"Which DLSS image model to use.")) {
			actions.reset = true;
			actions.persist = true;
		}

		if (state.have_motion_confidence) {
			ImGui::SliderFloat("Confidence mask", &settings.dlss_bias_strength, 0.0f, 1.0f, "%.2f");
			hint("Reduces trailing behind flames, dust and flickering lights.\n"
				"0 turns it off.");
			if (ImGui::IsItemDeactivatedAfterEdit())
				actions.persist = true;
		}
	}

	ImGui::SeparatorText("Output");
	ImGui::SliderFloat("Sharpness", &settings.sharpness, 0.0f, 1.0f, "%.2f");
	hint("How sharp the final image looks.");
	if (ImGui::IsItemDeactivatedAfterEdit())
		actions.persist = true;
}

void draw_neural_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	{
		if (ImGui::Checkbox("Enable neural rendering", &settings.neural_render)) {
			if (!settings.neural_render)
				actions.neural_restart = true;
			actions.persist = true;
		}
		hint("Adds fine detail to the finished image. Works with any upscaler, or on\n"
			"its own. NVIDIA only, and needs an extra runtime file - see Diagnostics\n"
			"if it will not start.");

		if (!vendor_may_be_nvidia(state.gpu_vendor)) {
			ImGui::TextDisabled("Neural rendering: unavailable (%s detected)",
				vendor_label(state.gpu_vendor));
			hint("Only this feature needs an NVIDIA card. Everything else works here.");
		}
		if (state.upscalers_remote) {
			ImGui::TextDisabled(state.host_ready
				? "Runs in AeonSRHost.exe, beside the game."
				: "Runs in AeonSRHost.exe, which starts with the first upscaled frame.");
			hint("This game is a 32-bit program and every upscaler is 64-bit, so they\n"
				"run in a small helper process next to it.");
		} else if (!state.neural_dll_found) {
			ImGui::TextDisabled("nvngx_dlssnr.dll: missing (supply your own)");
			hint((std::string("This file is not included and the driver does not install it.\n")
				+ "Copy it from a game you own into the folder holding " + kAddonFileName + ".").c_str());
		} else if (!state.neural_runtime_file.empty()) {
			const size_t cut = state.neural_runtime_file.find_last_of(L"\\/");
			const std::string name = narrow_lossy(cut == std::wstring::npos
				? state.neural_runtime_file : state.neural_runtime_file.substr(cut + 1));
			ImGui::TextDisabled("Runtime: %s, built for %s", name.c_str(),
				state.neural_runtime_cards.empty() ? "unknown cards" : state.neural_runtime_cards.c_str());
			hint("The neural rendering file in use and the cards it can run on.\n"
				"A build in runtime\\dlss5-allgpu\\ is tried first.");
		}
		const bool gpu_refused = vendor_may_be_nvidia(state.gpu_vendor) &&
			(state.neural_adapter_unsupported || state.neural_feature_unsupported ||
				!state.neural_runtime_serves_card ||
				state.neural_status == UpscalerStatus::UnsupportedGpu);
		if (gpu_refused) {
			ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
				"Neural rendering: NVIDIA's runtime will not run this pass on this GPU.");
			if (state.neural_adapter_unsupported) {
				ImGui::TextDisabled("It needs %s or newer. Everything else in AeonSR works here.",
					neural_arch_name(state.neural_min_architecture));
			} else if (!state.neural_runtime_serves_card && state.neural_gpu_architecture != 0) {
				ImGui::TextDisabled("This GPU is %s; this nvngx_dlssnr.dll is built for %s.",
					neural_arch_name(state.neural_gpu_architecture),
					state.neural_runtime_cards.empty() ? "other cards" : state.neural_runtime_cards.c_str());
				if (state.neural_allgpu_build_targets)
					ImGui::TextDisabled("A build for all GPUs runs here: put it in runtime\\dlss5-allgpu\\.");
				else
					ImGui::TextDisabled("No build of nvngx_dlssnr.dll runs on this GPU.");
			} else if (state.neural_gpu_architecture != 0) {
				ImGui::TextDisabled("This GPU is %s. Everything else in AeonSR works here.",
					neural_arch_name(state.neural_gpu_architecture));
			} else {
				ImGui::TextDisabled("Everything else in AeonSR works on this GPU.");
			}
			if (state.neural_runtime_serves_card)
				hint("Nothing on this panel changes this.");
			else if (state.neural_allgpu_build_targets)
				hint("Put a build of nvngx_dlssnr.dll made for this card in runtime\\dlss5-allgpu\\,\n"
					"then turn neural rendering off and on. Updates never overwrite that folder.");
			else
				hint("This card has no compatible nvngx_dlssnr.dll build. Everything else works.");
		}

		if (settings.neural_render) {
			const char *pass_state =
				state.neural_last == 1 ? "applied"
				: state.neural_crashed ? "FAILED: the neural rendering runtime faulted; pass disabled until reload"
				: state.neural_last == -1 && !gpu_refused ? "FAILED (see Detail below and AeonSR.log)"
				: gpu_refused ? "BLOCKED: the runtime will not run this pass on this GPU"
				: state.neural_driver_too_old ? "BLOCKED: driver older than the runtime requires"
				: !state.neural_shim_present ? "BLOCKED: ngxshim\\nvngx.dll missing (see Detail)"
				: settings.neural_no_motion_vectors ? "idle"
				: "idle (needs motion vectors)";
			const bool ok = state.neural_last == 1;
			ImGui::TextColored(ok ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.95f, 0.75f, 0.4f, 1.0f),
				"Neural pass: %s", pass_state);
			if (state.neural_last == 0 && !settings.neural_no_motion_vectors && !state.flow_note.empty())
				ImGui::TextWrapped("Why: %s", narrow_lossy(state.flow_note).c_str());
			if (!state.neural_error.empty())
				ImGui::TextWrapped("Detail: %s", narrow_lossy(state.neural_error).c_str());
			if (state.neural_required_driver_major != 0) {
				const bool old_driver = state.neural_driver_too_old;
				ImGui::TextColored(old_driver ? ImVec4(0.95f, 0.4f, 0.4f, 1.0f) : ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
					"Driver: %u.%02u  (runtime requires %u.%u)",
					state.neural_driver_major, state.neural_driver_minor,
					state.neural_required_driver_major, state.neural_required_driver_minor);
			}
			if (state.neural_gpu_architecture != 0) {
				if (state.neural_requirement_known && state.neural_min_architecture != 0)
					ImGui::TextDisabled("GPU: 0x%03X (%s)   runtime minimum: 0x%03X (%s)",
						state.neural_gpu_architecture,
						neural_arch_name(state.neural_gpu_architecture),
						state.neural_min_architecture,
						neural_arch_name(state.neural_min_architecture));
				else
					ImGui::TextDisabled("GPU: 0x%03X (%s)   runtime minimum: not answered",
						state.neural_gpu_architecture,
						neural_arch_name(state.neural_gpu_architecture));
				hint("Your card, and the oldest card this runtime file accepts.");
			}
			if (!state.neural_runtime_kernels.empty()) {
				ImGui::TextDisabled("Runtime built for: %s", state.neural_runtime_kernels.c_str());
				hint("Which graphics cards this runtime file supports. A card that is not\n"
					"on the list cannot run this feature.");
			}
			ImGui::Text("Evals: %llu%s", static_cast<unsigned long long>(state.neural_eval_count),
				state.neural_via_bridge ? "   (via D3D11on12 bridge)" : "");

			ImGui::SeparatorText("Detail and stability");
			{
				static const RowOption kScaleOptions[] = {
					{ "Full", "Sharpest, heaviest" },
					{ "90%", "" },
					{ "80%", "" },
					{ "70%", "" },
					{ "60%", "Good balance" },
					{ "50%", "" },
					{ "40%", "Cheapest, softest" },
				};
				if (button_row_index<kNeuralModelScaleCount>("Detail", &settings.neural_model_scale,
						kScaleOptions,
						"How much detail the effect can add. Higher looks sharper and\n"
						"costs more performance."))
					actions.persist = true;
				if ((settings.neural_model_scale != 0u || clamp_neural_mode(settings.neural_mode) != 0u) &&
						state.neural_model_width != 0)
					ImGui::TextDisabled("Working at %ux%u.",
						state.neural_model_width, state.neural_model_height);

				static const RowOption kModeOptions[] = {
					{ "Quality", "Full detail everywhere" },
					{ "Balanced", "Faster; full detail in the middle" },
					{ "Performance", "Faster still; less detail towards the edges" },
					{ "Ultra performance", "Fastest; least detail at the edges" },
				};
				if (button_row_index<kNeuralModeCount>("DLSS 5 Quality", &settings.neural_mode, kModeOptions,
						"Every mode updates the whole picture every frame. The faster\n"
						"ones keep full detail in the middle of the screen and spend\n"
						"less towards the edges."))
					actions.persist = true;

				if (ImGui::Checkbox("Smoothing", &settings.neural_smoothing))
					actions.persist = true;
				hint("Steadies the effect between frames and removes trails behind\n"
					"moving objects. Off shows the model's answer raw, which is\n"
					"sharper and restless.");
			}

			ImGui::SeparatorText("How much of it lands");

			ImGui::SliderFloat("Detail strength", &settings.neural_detail_strength,
				0.0f, kNeuralDetailMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			hint("How strong the added detail is. This is the main slider.\n"
				"If turning it up stops changing dark areas, raise Highlight guard.");
			if (ImGui::IsItemDeactivatedAfterEdit())
				actions.persist = true;
			ImGui::SliderFloat("Colour strength", &settings.neural_colour_strength,
				0.0f, kNeuralUnitMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			hint("How much of the effect's colour is used.\n"
				"0 keeps the game's own colours and changes only brightness.");
			if (ImGui::IsItemDeactivatedAfterEdit())
				actions.persist = true;

			ImGui::SeparatorText("Model");

			static const RowOption kStyleOptions[] = {
				{ "Standard", "Neutral" },
				{ "Natural", "Brighter, livelier - can look shiny" },
				{ "Cinematic", "Softer light" },
			};
			if (button_row_index<kNeuralStyleCount>("Look", &settings.neural_style, kStyleOptions,
					"Overall look of the effect."))
				actions.persist = true;

			ImGui::SliderFloat("Neural intensity", &settings.neural_intensity, 0.0f, kNeuralUnitMax, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
			hint("Strength of the effect itself. It can only reduce it -\n"
				"use Detail strength to push further.");
			if (ImGui::IsItemDeactivatedAfterEdit())
				actions.persist = true;
			ImGui::SliderFloat("Local structure", &settings.neural_local_structure, 0.0f, kNeuralExtendedMax, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
			hint("Amount of fine detail. 1.00 is the default.");
			if (ImGui::IsItemDeactivatedAfterEdit())
				actions.persist = true;
			ImGui::SliderFloat("Local tone", &settings.neural_local_tone, 0.0f, kNeuralExtendedMax, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
			hint("Local contrast. Lower it if the lighting reacts too strongly.");
			if (ImGui::IsItemDeactivatedAfterEdit())
				actions.persist = true;
			ImGui::SliderFloat("Skin structure", &settings.neural_skin_structure,
				kNeuralSkinMin, kNeuralSkinMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			hint("Detail on skin, set separately from the rest of the frame.");
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				settings.neural_skin_structure = clamp_neural_skin(settings.neural_skin_structure);
				actions.persist = true;
			}
			if (ImGui::Checkbox("Auto skin mask", &settings.neural_auto_mask))
				actions.persist = true;
			hint("Finds skin automatically for the slider above.");

			ImGui::SeparatorText("Colour");
			{
				const char *cs =
					state.neural_color_space == NeuralColorSpace::Pq ? "HDR10 (PQ)"
					: state.neural_color_space == NeuralColorSpace::ScrgbLinear ? "scRGB (linear)"
					: "SDR / sRGB";
				ImGui::Text("Frame colour space: %s", cs);
				hint("The colour format of the game's image. Detected automatically.");
			}
			ImGui::SliderFloat("Paper white", &settings.neural_paper_white,
				kNeuralPaperWhiteMin, kNeuralPaperWhiteMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			hint("Use this if highlights look blown out in HDR.\n"
				"1.00 changes nothing.");
			if (ImGui::IsItemDeactivatedAfterEdit())
				actions.persist = true;
			if (ImGui::Checkbox("Highlight guard", &settings.neural_highlight_guard_on))
				actions.persist = true;
			hint("Limits how much brighter the effect may make a pixel.\n"
				"Turn it on if bright edges glow; it costs some detail in shadows.");

			ImGui::SeparatorText("Inspect");
			{
				static const RowOption kDebugOptions[] = {
					{ "Off" },
					{ "Input", "What the effect is given" },
					{ "Output", "What the effect produces" },
					{ "Difference", "What it is changing, exaggerated" },
				};

				button_row_index<kNeuralDebugViewCount>("Neural debug view", &settings.neural_debug_view, kDebugOptions,
					"Shows one of the effect's internal images instead of the game.\n"
					"Not saved between sessions.");
				if (settings.neural_debug_view != 0u)
					ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.4f, 1.0f),
						"The game image is replaced by an inspection view.");
			}

			ImGui::SeparatorText("Guides");
			if (ImGui::Checkbox("Run without depth", &settings.neural_no_depth)) {
				actions.neural_restart = true;
				actions.persist = true;
			}
			hint("Skips the depth buffer. Slightly faster, no visible difference.");

			if (ImGui::Checkbox("Model without motion vectors", &settings.neural_no_motion_vectors)) {
				actions.neural_restart = true;
				actions.persist = true;
			}
			hint("Makes the effect judge each frame on its own.\n"
				"Removes smearing around moving objects, and costs performance.");

			ImGui::SeparatorText("Advanced");
			{
				static const RowOption kPassOptions[] = { { "1x" }, { "2x" }, { "3x" } };
				static const unsigned int kPassValues[] = { 1u, 2u, 3u };
				static_assert(IM_ARRAYSIZE(kPassValues) == static_cast<int>(kNeuralPassMax),
					"the row must offer every pass count up to kNeuralPassMax");
				if (button_row_value("Passes", &settings.neural_passes, kPassOptions, kPassValues,
						"Runs the effect more than once for stronger detail.\n"
						"Each extra pass costs as much as the first."))
					actions.persist = true;

				if (clamp_neural_passes(settings.neural_passes) > 1u &&
					settings.neural_model_scale > 1u)
					ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.4f, 1.0f),
						"Extra passes are wasted below 80%% Detail.\n"
						"Raise Detail, or set Passes back to 1x.");

				{
					const unsigned int cost_mode = clamp_neural_mode(settings.neural_mode);
					const float rows = cost_mode == 0u ? 1.0f
						: (state.neural_area_share > 0.0f ? state.neural_area_share
							: kNeuralModeKeep[cost_mode] * kNeuralModeKeep[cost_mode]);
					const float rel = neural_chain_relative_cost(
						neural_model_scale_factor(settings.neural_model_scale),
						settings.neural_passes, rows);
					if (state.neural_gpu_ms > 0.0f) {
						ImGui::Text("Neural pass: %.2f ms of GPU time per frame", state.neural_gpu_ms);
						hint("How long this effect takes each frame.\n"
							"A 60 fps frame has 16.7 ms in total, and the game needs most of it.");
					}
					ImGui::TextDisabled("These settings cost about %.2fx one pass at Full detail.", rel);

					const unsigned int built = state.neural_passes_built;
					if (built != 0 && built < clamp_neural_passes(settings.neural_passes))
						ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.4f, 1.0f),
							"Only %u pass%s could be created; that is what is running.",
							built, built == 1u ? "" : "es");
				}
			}

			if (ImGui::Checkbox("Reset on camera cut", &settings.neural_reset_on_cut))
				actions.persist = true;
			hint("Clears the effect's history on scene changes.\n"
				"Off by default - it can make the lighting jump.");

			{

				static const RowOption kKeyOptions[] = { { "None" }, { "F5" }, { "F6" }, { "F7" }, { "F8" } };
				static const unsigned int kKeyCodes[] = { 0u, 0x74u, 0x75u, 0x76u, 0x77u };
				if (button_row_value("Toggle key", &settings.neural_toggle_key, kKeyOptions, kKeyCodes,
						"Key that switches this effect on and off while playing."))
					actions.persist = true;
			}

			if (ImGui::Button("Restart neural pass")) {
				actions.neural_restart = true;
				actions.persist = true;
			}
			hint("Reloads the effect. Try this if it stops working.");
			ImGui::SameLine();
			if (ImGui::Button("Capture neural frame"))
				actions.neural_capture = true;
			hint("Saves a few uncompressed frames to the captures folder beside the\n"
				"add-on. Use these to compare image quality - a screen recording\n"
				"adds noise of its own.");
			if (!state.neural_capture_status.empty())
				ImGui::TextWrapped("Capture: %s", narrow_lossy(state.neural_capture_status).c_str());
		}
	}

}

void draw_experiments_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	ImGui::SeparatorText("Vendor profile");
	static const RowOption kUpscaleOptions[] = {
		{ "Native", "Full resolution - anti-aliasing only" },
		{ "Ultra Quality", "Not offered by every upscaler; falls back to Quality" },
		{ "Quality" },
		{ "Balanced" },
		{ "Performance" },
		{ "Ultra Performance" },
	};
	constexpr unsigned int kUpscaleModeCount = static_cast<unsigned int>(UpscaleMode::UltraPerformance) + 1u;
	if (button_row_index<kUpscaleModeCount>("Mode", &settings.upscale_mode, kUpscaleOptions,
			"How much the game renders before upscaling.\n"
			"Lower renders less and runs faster.")) {
		actions.reset = true;
		actions.invalidate_history = true;
		actions.persist = true;
	}

	if (settings.upscale_mode != 0) {
		const bool vendor = state.active_is_fsr || state.active_is_xess;
		ImGui::Text("Render: %ux%u -> %ux%u",
			vendor ? state.upscaler_render_width : state.ngx_render_width,
			vendor ? state.upscaler_render_height : state.ngx_render_height,
			vendor ? state.upscaler_out_width : state.ngx_width,
			vendor ? state.upscaler_out_height : state.ngx_height);
	}

	ImGui::SeparatorText("Screenshot");
	{
		static const RowOption kAccumOptions[] = {
			{ "2x" }, { "4x" }, { "8x", "A good place to start" },
			{ "16x" }, { "32x" }, { "64x", "Slowest, and it can look overcooked" },
		};
		static const unsigned int kAccumValues[] = { 2u, 4u, 8u, 16u, 32u, 64u };
		if (button_row_value("Detail passes", &settings.accum_iterations,
				kAccumOptions, kAccumValues,
				"How many times to run the effect over its own result."))
			actions.persist = true;

		const bool can_run = settings.neural_render && state.neural_last == 1 &&
			!state.upscalers_remote;
		ImGui::BeginDisabled(!can_run && !state.accum_running);
		if (!state.accum_running) {
			if (ImGui::Button("Freeze and build"))
				actions.accum_start = true;
		} else if (ImGui::Button("Release")) {
			actions.accum_stop = true;
		}
		ImGui::EndDisabled();
		if (state.upscalers_remote)
			ImGui::TextDisabled("Not available in this game: the neural pass runs in a helper process.");
		hint("Freezes the picture and keeps running the neural effect over its own\n"
			"result, one pass per frame, so the image gets as much detail as a long\n"
			"chain would without costing any extra performance.\n"
			"\n"
			"For screenshots only - the game keeps running underneath, but what you\n"
			"see is held still until you press Release.");

		if (state.accum_running && !state.accum_holding) {
			ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.4f, 1.0f),
				"Building: pass %u of %u", state.accum_done, state.accum_target);
		} else if (state.accum_holding) {
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
				"Held after %u passes - take the screenshot, then Release.",
				state.accum_done);
		} else if (!can_run) {
			ImGui::TextDisabled("Needs neural rendering to be on and running.");
		}

		if (settings.neural_model_scale > 1u)
			ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.4f, 1.0f),
				"Raise Detail to 80%% or Full first - below that the detail this\n"
				"builds is lost again. It costs nothing extra here:\n"
				"one pass a frame either way.");
	}

	ImGui::SeparatorText("Probe");
	if (ImGui::Checkbox("Probe the motion vectors", &settings.mv_probe)) {
		if (!settings.mv_probe)
			actions.probe_reset = true;
		actions.persist = true;
	}
	hint("Checks now and then that motion detection is working.");
	if (settings.mv_probe) {
		if (state.probe_unsupported) {
			ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
				"Probe: format cannot be read back (expected RG16F or RG32F).");
		} else if (!state.probe_valid) {
			ImGui::TextDisabled("Probe: waiting for the first sample...");
		} else {
			const bool dead = state.probe_nonzero_pct <= 0.0f;
			ImGui::TextColored(dead ? ImVec4(0.95f, 0.4f, 0.4f, 1.0f) : ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
				"Probe: %.0f%% non-zero, mean %.2f px, max %.2f px",
				state.probe_nonzero_pct, state.probe_mean_px, state.probe_max_px);
			if (dead && state.probe_moving)
				ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
					"Camera moved and every vector was zero: the provider writes nothing.");
		}
	}

}

ImVec4 check_colour(CheckState s)
{
	switch (s) {
	case CheckState::Ok: return ImVec4(0.40f, 0.90f, 0.40f, 1.0f);
	case CheckState::Warn: return ImVec4(0.95f, 0.75f, 0.40f, 1.0f);
	case CheckState::Fail: return ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
	case CheckState::Off: break;
	}
	return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
}

void draw_health(const PanelState &state, const Settings &settings)
{
	const std::vector<DiagCheck> checks = diag_checks(state, settings);
	for (const DiagCheck &c : checks) {
		ImGui::TextColored(check_colour(c.state), "[%s]", check_state_label(c.state));
		ImGui::SameLine();
		ImGui::Text("%-16s", c.name);
		ImGui::SameLine();
		ImGui::TextWrapped("%s", c.detail.c_str());
		if (!c.action.empty()) {
			ImGui::Indent();
			ImGui::TextDisabled("%s", c.action.c_str());
			ImGui::Unindent();
		}
	}
}

void draw_report_tools(const PanelState &state, Settings &settings, PanelActions &actions)
{
	if (ImGui::Button("Save report"))
		actions.save_report = true;
	hint("Writes AeonSR_report.txt beside the add-on: your GPU, driver, Windows,\n"
		"ReShade, the files found, every check above and the recent log.\n"
		"Attach it when reporting a problem.");
	ImGui::SameLine();
	if (ImGui::Button("Copy report"))
		actions.copy_report = true;
	hint("The same report, straight to the clipboard.");

	if (!state.report_error.empty())
		ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "%s",
			narrow_lossy(state.report_error).c_str());
	else if (!state.report_path.empty())
		ImGui::TextDisabled("Report: %s", narrow_lossy(state.report_path).c_str());

	if (ImGui::Checkbox("Detailed log", &settings.verbose_log))
		actions.persist = true;
	hint("Records every step of every frame into AeonSR.log.\n"
		"Turn it on only while reproducing a problem - it costs performance.");
	if (!state.log_path.empty())
		ImGui::TextDisabled("Log: %s", narrow_lossy(state.log_path).c_str());
}

void draw_recent_problems()
{
	const std::vector<DiagEntry> problems = diag_recent(8, DiagLevel::Warn);
	if (problems.empty()) {
		ImGui::TextDisabled("Nothing has failed this session.");
		return;
	}
	for (auto it = problems.rbegin(); it != problems.rend(); ++it) {
		ImGui::TextColored(it->level == DiagLevel::Error
			? ImVec4(0.95f, 0.4f, 0.4f, 1.0f) : ImVec4(0.95f, 0.75f, 0.4f, 1.0f),
			"%6.1fs  %s", static_cast<double>(it->time_s), it->tag.c_str());
		ImGui::SameLine();
		ImGui::TextWrapped("%s", narrow_lossy(it->text).c_str());
	}
}

void draw_diagnostics_section(const PanelState &state, Settings &settings, PanelActions &actions)
{
	ImGui::SeparatorText("Health");
	draw_health(state, settings);

	ImGui::SeparatorText("Report");
	draw_report_tools(state, settings, actions);

	ImGui::SeparatorText("Recent problems");
	draw_recent_problems();

	if (!ImGui::CollapsingHeader("Details"))
		return;

	ImGui::Text("Aeon SR %s   Build: %s   API: %s", state.version, state.build_stamp,
		state.api_name != nullptr ? state.api_name : "unknown");
	if (!state.gpu_name.empty())
		ImGui::Text("GPU: %s   (%s, 0x%04X)", narrow_lossy(state.gpu_name).c_str(),
			vendor_label(state.gpu_vendor), state.gpu_vendor_id);
	ImGui::Text("Motion: %s   Depth: %s   Camera: %.2f px",
		motion_provider_label(state.motion_provider),
		depth_provider_label(state.depth_provider),
		state.last_motion_px);
	if (state.depth_provider == DepthProvider::None) {
		ImGui::TextDisabled("No depth: pick the game's depth buffer in ReShade's own depth settings.");
	} else {
		ImGui::TextDisabled("Depth: %s%s%s, far %.0f%s", state.depth_reversed ? "reversed" : "normal",
			state.depth_upside_down ? ", upside down" : "", state.depth_mirrored ? ", mirrored" : "",
			state.depth_far_plane, state.depth_logarithmic ? ", logarithmic" : "");
		hint("Taken from ReShade's preprocessor definitions; updates when you apply them.");
		if (!state.depth_overridden.empty())
			ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.4f, 1.0f), "Different depth inside: %s",
				state.depth_overridden.c_str());
	}
	{
		if (state.flow_note.empty()) {
			ImGui::TextDisabled("Motion: publishing.");
		} else {
			ImGui::TextDisabled("Motion: none - %s", narrow_lossy(state.flow_note).c_str());
			hint("Every upscaler here needs motion to run.");
		}
		static const RowOption kFlowQuality[] = { { "Balanced" }, { "High" } };
		if (button_row_index<2>("Motion quality", &settings.internal_flow_quality, kFlowQuality,
				"How carefully motion is detected.\n"
				"High is more accurate and costs a little more.")) {
			actions.persist = true;
		}
	}

	if (state.active_is_fsr || state.active_is_xess) {
		const std::string label = !state.upscaler_label.empty() ? state.upscaler_label
			: std::string(state.active_is_fsr ? "FSR" : "XeSS");
		ImGui::Text("%s DLL: %s   init: %s   %ux%u -> %ux%u",
			label.c_str(),
			state.upscaler_dll_present ? "yes" : "no",
			state.upscaler_initialized ? "yes" : "no",
			state.upscaler_render_width, state.upscaler_render_height,
			state.upscaler_out_width, state.upscaler_out_height);
		if (state.active_is_fsr) {
			const std::wstring &running = state.fsr_provider_version;
			const bool running_4 = running.size() > 1 && running[0] == L'4' && running[1] == L'.';
			bool offers_4 = false;
			for (const std::string &p : state.fsr_providers)
				offers_4 = offers_4 || (p.size() > 1 && p[0] == '4' && p[1] == '.');

			if (running.empty())
				ImGui::TextDisabled("The runtime did not say which FSR version it used.");
			if (offers_4 && !running_4) {
				ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.4f, 1.0f),
					"This runtime offers FSR 4, and fell back to %s on this GPU.",
					running.empty() ? "an older version" : narrow_lossy(running).c_str());
				hint("FSR 4 runs on RDNA 4, and on RDNA 3 desktop cards with a recent\n"
					"Adrenalin driver. Everything else gets FSR 3.1.");
			}
		}
	} else if (state.active_is_vsr) {
		ImGui::Text("RTX VSR: capable %s  enhancement %s  in use %s  level %u   %ux%u -> %ux%u",
			state.vsr_capable ? "yes" : "no",
			state.vsr_enabled ? "on" : "off",
			state.vsr_in_use ? "yes" : "no", state.vsr_level,
			state.upscaler_render_width, state.upscaler_render_height,
			state.upscaler_out_width, state.upscaler_out_height);
	} else {
		ImGui::Text("DLSS DLL: %s   init: %s   %ux%u",
			state.ngx_dll_present ? "yes" : "no",
			state.ngx_initialized ? "yes" : "no",
			state.ngx_width, state.ngx_height);
	}

	if (state.bridge_kind != BridgeKind::Native12 && state.bridge_name != nullptr) {
		ImGui::TextDisabled("%s bridge: %s%s", state.api_name != nullptr ? state.api_name : "graphics",
			state.bridge_any_ready ? state.bridge_name : "not ready",
			state.bridge_fsr_adapter_matched ? "" : " (adapter not matched yet)");
		hint("Every upscaler here is DirectX 12 work. Set up automatically so a\n"
			"game on any other graphics API can use them, and motion detection.");
		if (state.bridge_sync != nullptr) {
			ImGui::SameLine();
			ImGui::TextDisabled("[%s]", state.bridge_sync);
		}
	} else if (state.bridge_name == nullptr && !state.bridge_error.empty()) {
		ImGui::TextDisabled("No bridge for this graphics API.");
		hint("The add-on could not reach a DirectX 12 device from this game.");
	}

	if (!state.game_ngx_modules.empty())
		ImGui::TextWrapped("The game already uses DLSS (%s); running a second one may be unstable.",
			narrow_lossy(state.game_ngx_modules).c_str());
	if (!state.game_fsr_modules.empty())
		ImGui::TextWrapped("The game already uses FSR (%s); running a second one may be unstable.",
			narrow_lossy(state.game_fsr_modules).c_str());
	if (!state.game_xess_modules.empty())
		ImGui::TextWrapped("The game already uses XeSS (%s); running a second one may be unstable.",
			narrow_lossy(state.game_xess_modules).c_str());

	ImGui::Separator();
	static const RowOption kDebugViewOptions[] = {
		{ "Off" },
		{ "Flow", "Motion as colour: hue is direction, brightness is speed" },
		{ "Arrows", "Motion as arrows over the game - the readable one" },
		{ "Confidence", "How sure the motion is: red unsure, green certain" },
		{ "Depth", "The depth the upscalers are given" },
	};
	if (button_row_index<kDebugViewCount>("Debug view", &settings.debug_view, kDebugViewOptions,
			"Shows what the add-on sees instead of the game.\n"
			"Nothing else changes."))
		actions.persist = true;
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
		ImGui::TextColored(ok ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
			: waiting ? ImVec4(0.85f, 0.8f, 0.4f, 1.0f) : ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
			"Debug view: %s", stage);
		if (state.debug_view_note != nullptr && state.debug_view_note[0] != 0)
			ImGui::TextDisabled("%s", state.debug_view_note);
	}
}

}

void draw_overlay(const PanelState &state, Settings &settings, PanelActions &actions)
{
	bool enabled = settings.enabled;
	if (ImGui::Checkbox("Enable upscaler", &enabled)) {
		settings.enabled = enabled;
		actions.reset = true;
		actions.persist = true;
	}
	hint("Turns the upscaler on. Neural rendering has its own switch.");

	static const RowOption kBackendOptions[] = {
		{ "DLSS", "NVIDIA only" },
		{ "FSR" },
		{ "XeSS" },
		{ "None", "Leaves the image as the game rendered it" },
		{ "RTX VSR", "NVIDIA only, DirectX 11 and SDR. Needs RTX video enhancement\n"
			"switched on in the NVIDIA Control Panel." },
	};
	if (button_row_index<kBackendChoiceCount>("Upscaler", &settings.backend, kBackendOptions,
			"Which upscaler to use. None runs only the neural pass.")) {
		actions.persist = true;
		actions.reset = true;
	}

	if (settings.backend == static_cast<unsigned>(BackendChoice::None)) {
		ImGui::TextDisabled("Active: none - the frame is left as the game rendered it.");
	} else if (!state.upscaler_label.empty() || state.active_name != nullptr) {
		ImGui::Text("Active: %s   Status: %s",
			!state.upscaler_label.empty() ? state.upscaler_label.c_str() : state.active_name,
			status_label(state.active_status));
	}
	if (!state.active_error.empty())
		ImGui::TextWrapped("Detail: %s", narrow_lossy(state.active_error).c_str());

	if (state.native_dlss) {
		ImGui::TextDisabled("Game's own DLSS: yes (%s)",
			narrow_lossy(state.native_dlss_modules).c_str());
		if (state.host_ready)
			ImGui::TextDisabled("Aeon SR's DLSS runs in AeonSRHost.exe, apart from the game's.");
	} else {
		ImGui::TextDisabled("Game's own DLSS: none seen.");
	}
		hint("If the game has its own DLSS, use that one and set the upscaler here\n"
			"to None. Two upscalers in a row cause trailing.");

	if (state.active_is_fsr && state.fsr_providers.size() > 1) {
		std::vector<RowOption> opts;
		opts.push_back(RowOption{ "Auto", "Picks the best version this GPU can run" });
		for (const std::string &pv : state.fsr_providers)
			opts.push_back(RowOption{ pv.c_str(), nullptr });
		int idx = 0;
		for (size_t i = 0; i < state.fsr_providers.size(); ++i) {
			if (state.fsr_providers[i] == settings.fsr_provider)
				idx = static_cast<int>(i) + 1;
		}
		if (button_row_n("FSR version", &idx, opts.data(), static_cast<int>(opts.size()),
				"Locks FSR to one version instead of letting it choose.")) {
			settings.fsr_provider = idx == 0 ? std::string() : state.fsr_providers[static_cast<size_t>(idx) - 1];
			actions.persist = true;
			actions.reset = true;
		}
	}

	if (state.active_is_vsr && !state.vsr_possible)
		ImGui::TextDisabled("RTX VSR runs through the DirectX 11 video processor, so it is only "
			"available in a DirectX 11 game. Use DLSS, FSR or XeSS.");
	else if (state.active_is_vsr)
		ImGui::TextDisabled("Driver: %s  %s  in use: %s  level %u",
			state.vsr_capable ? "VSR capable" : "not VSR capable",
			state.vsr_enabled ? "enhancement on" : "enhancement OFF (Control Panel)",
			state.vsr_in_use ? "yes" : "no", state.vsr_level);

	if (ImGui::Checkbox("Show on-screen status", &settings.show_osd))
		actions.persist = true;
	hint("Small status readout drawn over the game.");

	if (ImGui::CollapsingHeader("Quality", ImGuiTreeNodeFlags_DefaultOpen))
		draw_quality_section(state, settings, actions);
	if (ImGui::CollapsingHeader("Neural rendering (experimental)"))
		draw_neural_section(state, settings, actions);
	if (ImGui::CollapsingHeader("Experiments"))
		draw_experiments_section(state, settings, actions);
	{
		const CheckState worst = diag_worst(diag_checks(state, settings));
		char title[64]{};
		snprintf(title, sizeof(title), "Diagnostics%s###diagnostics",
			worst == CheckState::Fail ? "  (something failed)"
				: worst == CheckState::Warn ? "  (check this)" : "");
		if (worst != CheckState::Ok && worst != CheckState::Off)
			ImGui::PushStyleColor(ImGuiCol_Text, check_colour(worst));
		const bool open = ImGui::CollapsingHeader(title);
		if (worst != CheckState::Ok && worst != CheckState::Off)
			ImGui::PopStyleColor();
		if (open)
			draw_diagnostics_section(state, settings, actions);
	}

	ImGui::Spacing();
	if (ImGui::Button("Reset temporal history"))
		actions.reset = true;
	hint("Clears what the upscaler remembers from previous frames.\n"
		"Try this after a glitch.");
}

void draw_osd(const PanelState &state, const Settings &settings)
{
	if (!settings.show_osd)
		return;

	const bool d3d12 = state.d3d12;

	const bool no_upscaler = !settings.enabled ||
		settings.backend == static_cast<unsigned>(BackendChoice::None);
	ImGui::Text("Aeon SR: %s", no_upscaler ? "no upscaler"
		: !state.upscaler_label.empty() ? state.upscaler_label.c_str()
		: (state.active_name != nullptr ? state.active_name : "—"));
	if (settings.enabled && !no_upscaler)
		ImGui::Text("Status: %s", status_label(state.active_status));
	if (settings.enabled && !state.active_error.empty())
		ImGui::TextWrapped("%s", narrow_lossy(state.active_error).c_str());
	if (settings.neural_render) {
		ImGui::Text("Neural (DLSSNR): %s",
			!vendor_may_be_nvidia(state.gpu_vendor) ? "needs NVIDIA"
			: state.neural_crashed ? "failed"
			: !state.neural_shim_present ? "no shim"
			: state.neural_last == 1 ? "on"
			: state.neural_last == -1 ? "error"
			: state.neural_dll_present ? "idle" : "no DLL");
	}

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

	ImGui::SeparatorText("About");
	{
		ImGui::TextDisabled("Aeon SR - Barbatos AWLS");
		ImGui::TextDisabled("Powered by NVIDIA DLSS and the NVIDIA NGX SDK.");
		ImGui::TextDisabled("NVIDIA, NVIDIA RTX and GeForce RTX are trademarks of NVIDIA Corporation.");
		ImGui::TextDisabled("AMD FidelityFX Super Resolution (c) Advanced Micro Devices, Inc. - MIT.");
		ImGui::TextDisabled("Intel XeSS (c) Intel Corporation - Intel Simplified Software License.");
		ImGui::TextDisabled("Full terms in THIRD-PARTY-NOTICES.md beside the add-on.");
	}
}

}
}

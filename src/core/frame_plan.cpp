#include "aeon_sr/core/frame_plan.hpp"

namespace aeon_sr {

NeuralRenderParams plan_neural_params(const Settings &s, const FrameSignals &sig, bool reset,
	float frame_time_ms) noexcept
{
	NeuralRenderParams np;
	np.style = clamp_neural_style(s.neural_style);

	np.intensity = clamp_neural_unit(s.neural_intensity, kNeuralIntensityDefault, kNeuralUnitMax);
	np.local_structure = clamp_neural_unit(s.neural_local_structure, kNeuralStructureDefault, kNeuralExtendedMax);
	np.local_tone = clamp_neural_unit(s.neural_local_tone, kNeuralToneDefault, kNeuralExtendedMax);

	np.skin_structure = clamp_neural_skin(s.neural_skin_structure);
	np.auto_mask = s.neural_auto_mask;

	np.passes = clamp_neural_passes(s.neural_passes);
	np.mode = static_cast<uint8_t>(clamp_neural_mode(s.neural_mode));

	np.depth_inverted = false;

	np.reset = reset;

	np.feed_runtime_motion = !s.neural_no_motion_vectors;

	np.detail_strength = clamp_neural_unit(s.neural_detail_strength, 1.0f, kNeuralDetailMax);
	np.colour_strength = clamp_neural_unit(s.neural_colour_strength, 1.0f, kNeuralUnitMax);
	np.model_scale = neural_model_scale_factor(s.neural_model_scale);
	const float blend = s.neural_smoothing ? s.neural_smooth_blend : 0.0f;
	np.temporal_alpha = neural_blend_alpha(blend);
	np.smooth_weight = neural_steady_weight(s.neural_smooth_steady, blend);
	np.carry_reject = neural_reject_threshold(s.neural_carry_reject);
	np.scale_compensation = neural_scale_compensation(s.neural_model_scale);
	np.paper_white = clamp_neural_range(s.neural_paper_white, kNeuralPaperWhiteMin, kNeuralPaperWhiteMax, 1.0f);
	np.highlight_guard = s.neural_highlight_guard_on
		? clamp_neural_range(s.neural_highlight_guard,
			kNeuralHighlightGuardMin, kNeuralHighlightGuardMax, 2.0f)
		: kNeuralHighlightGuardOff;
	np.debug_view = s.neural_debug_view < kNeuralDebugViewCount ? s.neural_debug_view : 0u;
	np.color_space = sig.color_space;
	np.frame_time_ms = frame_time_ms;
	return np;
}

void plan_vsr_params(const Settings &s, const FrameSignals &sig, UpscalerParams &p) noexcept
{

	p.render_scale = vsr_scale_for_mode(static_cast<UpscaleMode>(s.upscale_mode));
	p.render_width = 0;
	p.render_height = 0;
	p.quality_mode = s.upscale_mode;
	p.sharpness = s.sharpness;

	p.jitter_x = 0.0f;
	p.jitter_y = 0.0f;
	p.jitter_in_frame = false;
	p.reset = false;
	p.vsr_input_format = s.vsr_input_format < kVsrInputFormatCount ? s.vsr_input_format : 0u;
	p.color_space = static_cast<uint32_t>(sig.color_space);

	p.depth_inverted = false;
	p.bias_mask = 0;
}

bool split_uses_catmull_rom(bool setting, BackendChoice backend) noexcept
{
	return setting || backend == BackendChoice::Dlss;
}

const char *jitter_decision_name(JitterDecision d) noexcept
{
	switch (d) {
	case JitterDecision::Drawn: return "drawn";
	case JitterDecision::Split: return "split";
	case JitterDecision::Ambiguous: return "ambiguous";
	case JitterDecision::Empty: return "empty";
	case JitterDecision::Resampled: return "resampled";
	case JitterDecision::Off: break;
	}
	return "off";
}

Jitter split_pair_point(uint32_t drawn_index, float amount) noexcept
{
	return halton_jitter(drawn_index / 2u + jitter_phases() / 2u, amount);
}

FramePlan plan_frame(const Settings &s, const FrameSignals &sig, const BackendSlots &slots,
	const FrameHistory &h, float frame_time_ms) noexcept
{
	FramePlan plan;
	plan.next = h;

	plan.neural.want_motion = !s.neural_no_motion_vectors;
	plan.neural.want_depth = !s.neural_no_depth;
	plan.neural.run = s.neural_render && (!plan.neural.want_motion || sig.have_motion_vectors);

	const BackendChoice choice = s.enabled
		? backend_choice(s.backend, slots.vendor)
		: BackendChoice::None;

	if (!sig.have_motion_vectors) {

		if (choice == BackendChoice::Vsr) {
			plan.outcome = UpscalerOutcome::Run;
			plan.backend_index = 3;
			plan_vsr_params(s, sig, plan.params);
			plan.params.frame_time_ms = frame_time_ms;
			plan.neural.params = plan_neural_params(s, sig, false, frame_time_ms);
			plan.next.reset_pending = false;
			return plan;
		}

		plan.outcome = UpscalerOutcome::NoMotionVectors;
		plan.params.frame_time_ms = frame_time_ms;
		plan.neural.params = plan_neural_params(s, sig, false, frame_time_ms);
		return plan;
	}

	UpscalerParams &p = plan.params;

	p.depth_inverted = false;

	p.quality_mode = s.upscale_mode;
	p.render_preset = s.render_preset;
	p.sharpness = s.sharpness;
	p.frame_time_ms = frame_time_ms;
	p.color_space = static_cast<uint32_t>(sig.color_space);

	const bool cut = camera_cut_detected(sig.have_global_flow, h.last_motion_px, h.prev_motion_px);
	plan.camera_cut = cut;

	if (sig.jitter_drawn) {
		p.jitter_x = -sig.jitter_drawn_x;
		p.jitter_y = -sig.jitter_drawn_y;
		p.jitter_in_frame = true;
		plan.jitter_active = true;
		plan.jitter_decision = JitterDecision::Drawn;
		if ((sig.jitter_drawn_index & 1u) != 0u) {
			const Jitter q = split_pair_point(sig.jitter_drawn_index, kJitterAmount);
			plan.split_shift_x = q.x - sig.jitter_drawn_x;
			plan.split_shift_y = q.y - sig.jitter_drawn_y;
			p.jitter_x = -q.x;
			p.jitter_y = -q.y;
			plan.jitter_decision = JitterDecision::Split;
		}
		plan.next.jitter_index = (sig.jitter_drawn_index > h.jitter_index
			? sig.jitter_drawn_index : h.jitter_index) + 1u;
	} else if (sig.jitter_drawn_ambiguous) {
		p.jitter_in_frame = true;
		plan.jitter_active = true;
		plan.jitter_decision = JitterDecision::Ambiguous;
		plan.next.jitter_index = h.jitter_index + 1u;
	} else if (sig.jitter_drawn_empty) {
		p.jitter_in_frame = true;
		plan.jitter_active = true;
		plan.jitter_decision = JitterDecision::Empty;
		plan.next.jitter_index = h.jitter_index + 1u;
	} else if (s.spatial_jitter) {
		const Jitter j = halton_jitter(h.jitter_index, kJitterAmount);
		p.jitter_x = j.x;
		p.jitter_y = j.y;
		plan.jitter_active = true;
		plan.jitter_decision = JitterDecision::Resampled;
		plan.next.jitter_index = h.jitter_index + 1u;
	}

	p.reset = h.reset_pending;
	plan.scene_state = (p.reset || cut) ? SceneState::Cut : SceneState::Moving;

	plan.invalidate_temporal_history = cut || h.reset_pending || p.reset;
	if (plan.invalidate_temporal_history)
		plan.next.temporal_history_valid = false;
	plan.next.prev_motion_px = h.last_motion_px;

	const bool neural_reset = h.reset_pending || (s.neural_reset_on_cut && cut);
	plan.neural.params = plan_neural_params(s, sig, neural_reset, frame_time_ms);

	if (!slots.is_d3d12 && slots.fsr_bridge_failed && slots.fsr11.present)
		plan.use_fsr11 = true;
	plan.backend_index = pick_backend(choice);

	if (plan.backend_index == 3)
		plan_vsr_params(s, sig, p);

	if (plan.backend_index < 0) {
		plan.outcome = UpscalerOutcome::NoBackend;
		plan.no_upscaler_wanted = choice == BackendChoice::None;
		plan.next.reset_pending = false;
		return plan;
	}
	plan.outcome = UpscalerOutcome::Run;
	plan.next.reset_pending = false;
	return plan;
}

}

#pragma once

#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

namespace aeon_sr {

enum class NeuralColorSpace : unsigned int { Sdr = 0, ScrgbLinear, Pq };

enum class AccumStep : unsigned char {
	Off,
	First,
	Next,
	Hold,
};

struct ScreenshotAccum {
	unsigned int target = 0;
	unsigned int done = 0;
	bool running = false;

	void start(unsigned int iterations) noexcept
	{
		target = clamp_accum_iterations(iterations);
		done = 0;
		running = true;
	}

	void stop() noexcept
	{
		running = false;
		target = 0;
		done = 0;
	}

	AccumStep step() const noexcept
	{
		if (!running || target == 0)
			return AccumStep::Off;
		if (done == 0)
			return AccumStep::First;
		return done < target ? AccumStep::Next : AccumStep::Hold;
	}

	void advance() noexcept
	{
		if (running && done < target)
			++done;
	}

	bool active() const noexcept { return step() != AccumStep::Off; }
	bool evaluating() const noexcept
	{
		const AccumStep s = step();
		return s == AccumStep::First || s == AccumStep::Next;
	}
};

struct NeuralRenderParams {
	unsigned int style = 0;

	float intensity = kNeuralIntensityDefault;
	float local_structure = kNeuralStructureDefault;
	float local_tone = kNeuralToneDefault;
	float skin_structure = kNeuralSkinDefault;

	static constexpr bool ui_correction = true;
	bool auto_mask = true;

	unsigned int render_preset = 0;
	unsigned int passes = 1u;
	bool depth_inverted = false;
	bool reset = false;

	bool feed_runtime_motion = true;
	uint8_t mode = 0u;

	float detail_strength = 1.0f;
	float colour_strength = 1.0f;
	float model_scale = 1.0f;

	float temporal_alpha = 1.0f;
	float smooth_weight = 0.2f;
	float carry_reject = 0.5f;
	float scale_compensation = 1.0f;
	float paper_white = 1.0f;
	float highlight_guard = 2.0f;
	unsigned int debug_view = 0;

	float frame_time_ms = 0.0f;

	NeuralColorSpace color_space = NeuralColorSpace::Sdr;

};

}

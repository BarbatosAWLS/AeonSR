#pragma once

#include "aeon_sr/motion/flow_probe.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/motion/mv_probe.hpp"
#include "aeon_sr/core/pass_context.hpp"
#include "aeon_sr/core/settings.hpp"

#include <cstdint>

namespace aeon_sr {

struct InputProbes {
	MvProbeResult mv;
	bool mv_moving = false;

	uint64_t frame = 0;

	void tick_motion(const PassContext &ctx, const Settings &s, const FrameInputs &in, float last_motion_px);

	bool sample_flow(const PassContext &ctx, const FrameInputs &in, float &motion_px);

	bool motion_write_due() const noexcept { return mv11_.write_due(frame + 1u); }
	void note_motion_written() noexcept { mv11_.note_written(frame + 1u); }

	void reset_motion();
	void release();

private:
	MvProbe11 mv11_;
	MvProbe12 mv12_;
	FlowProbe11 flow_;
};

}

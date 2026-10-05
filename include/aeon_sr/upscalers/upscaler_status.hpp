#pragma once

namespace aeon_sr {

enum class UpscalerStatus {
	Idle,
	MissingRuntime,
	UnsupportedApi,

	UnsupportedGpu,
	InitFailed,
	NeedMotionVectors,
	Ready,
	EvaluateFailed,
	Crashed,
	NeedDepth,
	Loading,
};

}

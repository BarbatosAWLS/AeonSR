#pragma once

#include "aeon_sr/ngx/neural_params.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <cstddef>
#include <cstdint>

namespace aeon_sr::remote {

constexpr uint32_t kProtocolVersion = 8;
constexpr uint32_t kMagic = 0x52535241u;

constexpr uint32_t kNameChars = 96;
constexpr uint32_t kTextChars = 512;

enum class ShareKind : uint32_t {
	None = 0,
	Name = 1,
	KmtValue = 2,
};

enum class PlaneRole : uint32_t {
	Colour = 0,
	Depth = 1,
	Motion = 2,
	Guide = 3,
	Count = 4,
};

const char *plane_role_label(PlaneRole role) noexcept;

struct PlaneDesc {
	uint32_t share;
	uint32_t dxgi_format;
	uint32_t width;
	uint32_t height;
	uint64_t kmt_value;
	uint64_t generation;
	wchar_t name[kNameChars];
};

enum class RemoteStep : uint32_t {
	Ok = 0,
	NoHost = 1,
	Handshake = 2,
	Timeout = 3,
	OpenPlanes = 4,
	Engine = 5,
	Upscaler = 6,
	Lost = 7,
	Share = 8,
};

const char *remote_step_label(RemoteStep step) noexcept;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct alignas(16) Block {
	uint32_t magic;
	uint32_t version;
	uint32_t block_size;
	uint32_t game_pid;

	uint32_t host_pid;
	uint32_t host_ready;
	uint32_t host_version_major;
	uint32_t host_version_minor;

	uint32_t backends_present;
	uint32_t neural_present;
	uint32_t host_gpu_vendor;
	uint32_t reserved_handshake;

	uint64_t frame_index;
	uint64_t response_index;

	uint32_t game_api;
	uint32_t plane_count;
	uint32_t adapter_luid_low;
	int32_t adapter_luid_high;

	uint32_t width;
	uint32_t height;

	PlaneDesc planes[static_cast<uint32_t>(PlaneRole::Count)];

	uint32_t backend_choice;
	uint32_t neural_run;
	uint32_t neural_want_depth;
	uint32_t have_motion;
	uint32_t have_depth;
	uint32_t have_guide;
	uint32_t shutdown;
	uint32_t guide_state;
	float depth_jitter_u;
	float depth_jitter_v;
	uint32_t reserved_request[2];

	UpscalerParams upscaler;
	NeuralRenderParams neural;

	wchar_t fence_name[kNameChars];
	uint64_t fence_wait_value;
	uint64_t fence_signal_value;

	uint32_t step;
	uint32_t backend_ran;
	uint32_t upscaler_status;
	uint32_t neural_ran;
	uint32_t upscaler_out_width;
	uint32_t upscaler_out_height;
	uint32_t neural_model_width;
	uint32_t neural_model_height;
	uint32_t neural_eval_rows;
	uint32_t neural_passes_built;
	uint32_t neural_gpu_us;
	uint32_t upscaler_in_size;
	wchar_t message[kTextChars];
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

constexpr uint32_t kBlockSize = 2464;
static_assert(sizeof(PlaneDesc) == 224, "PlaneDesc changed shape; bump kProtocolVersion");
static_assert(sizeof(UpscalerParams) == 64, "UpscalerParams crosses the wire; bump kProtocolVersion");
static_assert(sizeof(NeuralRenderParams) == 84, "NeuralRenderParams crosses the wire; bump kProtocolVersion");
static_assert(offsetof(UpscalerParams, bias_mask) == 56, "UpscalerParams moved; bump kProtocolVersion");
static_assert(offsetof(NeuralRenderParams, color_space) == 80, "NeuralRenderParams moved; bump kProtocolVersion");
static_assert(offsetof(NeuralRenderParams, mode) == 35, "NeuralRenderParams moved; bump kProtocolVersion");
static_assert(offsetof(Block, neural_model_width) == offsetof(Block, upscaler_out_height) + 4,
	"the host's status moved; bump kProtocolVersion");
static_assert(offsetof(Block, message) == offsetof(Block, neural_model_width) + 24,
	"the host's status moved; bump kProtocolVersion");
static_assert(sizeof(Block) == kBlockSize, "Block changed shape; bump kProtocolVersion and kBlockSize");
static_assert(sizeof(wchar_t) == 2, "the name fields assume UTF-16 on both sides");

inline void depth_jitter_to_read(const Block &block, uint32_t backend_ran, float *u, float *v) noexcept
{
	const bool upscaled = backend_ran == static_cast<uint32_t>(BackendChoice::Dlss) ||
		backend_ran == static_cast<uint32_t>(BackendChoice::Fsr) ||
		backend_ran == static_cast<uint32_t>(BackendChoice::Xess);
	*u = upscaled ? block.depth_jitter_u : 0.0f;
	*v = upscaled ? block.depth_jitter_v : 0.0f;
}

struct SessionNames {
	wchar_t block[kNameChars];
	wchar_t request[kNameChars];
	wchar_t response[kNameChars];
};

void session_names(uint32_t game_pid, SessionNames &out) noexcept;

void object_name(uint32_t game_pid, const wchar_t *what, uint64_t generation,
	wchar_t *out, uint32_t out_chars) noexcept;

constexpr uint32_t kFrameTimeoutMs = 2000;
constexpr uint32_t kFirstFrameTimeoutMs = 20000;
constexpr uint32_t kHandshakeTimeoutMs = 15000;

}

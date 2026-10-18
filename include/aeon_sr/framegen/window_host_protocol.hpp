#pragma once

#include <cstddef>
#include <cstdint>

namespace aeon_sr::window_host {

constexpr uint32_t kMagic = 0x57475341u;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kShared = 3;
constexpr uint32_t kTextChars = 256;

enum class Command : uint32_t {
	None = 0,
	Present = 1,
	Window = 2,
	Quit = 3,
};

enum class HostState : uint32_t {
	Starting = 0,
	Ready = 1,
	Failed = 2,
};

enum class ColorSpace : uint32_t {
	Sdr = 0,
	Scrgb = 1,
	Pq = 2,
};

struct Block {
	uint32_t magic;
	uint32_t version;
	uint32_t block_size;
	uint32_t game_pid;

	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t color_space;
	uint32_t adapter_luid_low;
	int32_t adapter_luid_high;
	uint32_t shared_count;
	uint32_t reserved0;
	uint64_t shared[kShared];
	uint64_t ready_fence;
	uint64_t done_fence;
	uint64_t game_process;
	uint64_t request_event;
	uint64_t response_event;
	uint64_t released_event;

	uint32_t host_pid;
	uint32_t host_state;
	uint32_t host_window;
	uint32_t reserved1;

	uint64_t seq;
	uint32_t command;
	uint32_t shared_index;
	uint64_t ready_value;
	uint64_t done_value;
	uint32_t sync_interval;
	uint32_t visible;
	int32_t x, y, cw, ch;

	uint64_t answered_seq;
	int32_t present_hr;
	uint32_t present_count;
	uint32_t stats_valid;
	uint32_t stats_present_count;
	uint32_t stats_present_refresh_count;
	uint32_t stats_sync_refresh_count;
	int64_t stats_sync_qpc;
	uint32_t window_shown;
	uint32_t reserved2;
	wchar_t message[kTextChars];
};

constexpr uint32_t kBlockSize = 752;
static_assert(sizeof(Block) == kBlockSize, "window_host::Block changed shape; bump kVersion and kBlockSize");
static_assert(offsetof(Block, shared) == 48, "window_host::Block moved; bump kVersion");
static_assert(offsetof(Block, seq) == 136, "window_host::Block moved; bump kVersion");
static_assert(offsetof(Block, answered_seq) == 192, "window_host::Block moved; bump kVersion");
static_assert(offsetof(Block, message) == 240, "window_host::Block moved; bump kVersion");
static_assert(sizeof(wchar_t) == 2, "the text field assumes UTF-16");

struct Setup {
	uint32_t width = 0, height = 0, format = 0;
	ColorSpace color_space = ColorSpace::Sdr;
	uint32_t adapter_luid_low = 0;
	int32_t adapter_luid_high = 0;
};

bool read_setup(const Block &block, Setup *out, const wchar_t **why) noexcept;

struct Request {
	uint64_t seq = 0;
	Command command = Command::None;
	uint32_t shared_index = 0;
	uint64_t ready_value = 0;
	uint64_t done_value = 0;
	uint32_t sync_interval = 1;
	bool visible = false;
	int32_t x = 0, y = 0, cw = 0, ch = 0;
};

bool read_request(const volatile Block &block, Request *out) noexcept;

bool format_presentable(uint32_t format) noexcept;

}

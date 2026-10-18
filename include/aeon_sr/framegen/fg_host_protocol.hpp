#pragma once

#include <cstddef>
#include <cstdint>

namespace aeon_sr::fg_host {

constexpr uint32_t kMagic = 0x47465341u;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxOutputs = 5;
constexpr uint32_t kTextChars = 256;
constexpr uint32_t kVersionChars = 64;

enum class Command : uint32_t {
	None = 0,
	Frame = 1,
	Reset = 2,
	Quit = 3,
};

enum class HostState : uint32_t {
	Starting = 0,
	Ready = 1,
	Failed = 2,
};

enum class Step : uint32_t {
	Generated = 0,
	Warming = 1,
	Building = 2,
	Unavailable = 3,
	Failed = 4,
	HostFailed = 5,
};

struct Block {
	uint32_t magic;
	uint32_t version;
	uint32_t block_size;
	uint32_t game_pid;

	uint32_t generator;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t adapter_luid_low;
	int32_t adapter_luid_high;
	uint32_t output_count;
	uint32_t reserved0;
	uint64_t color;
	uint64_t motion;
	uint64_t depth;
	uint64_t outputs[kMaxOutputs];
	uint64_t to_host;
	uint64_t to_game;
	uint64_t game_process;
	uint64_t request_event;
	uint64_t response_event;

	uint32_t host_pid;
	uint32_t host_state;
	uint32_t reserved1[2];

	uint64_t seq;
	uint32_t command;
	uint32_t generated;
	uint64_t wait_value;
	uint64_t signal_value;
	uint32_t reset;
	uint32_t color_space;
	float frame_ms;
	uint32_t window;

	uint64_t answered_seq;
	uint32_t step;
	uint32_t max_generated;
	uint32_t busy;
	uint32_t reserved2;
	wchar_t vendor_version[kVersionChars];
	wchar_t message[kTextChars];
};

constexpr uint32_t kBlockSize = 880;
static_assert(sizeof(Block) == kBlockSize, "fg_host::Block changed shape; bump kVersion and kBlockSize");
static_assert(offsetof(Block, color) == 48, "fg_host::Block moved; bump kVersion");
static_assert(offsetof(Block, seq) == 168, "fg_host::Block moved; bump kVersion");
static_assert(offsetof(Block, answered_seq) == 216, "fg_host::Block moved; bump kVersion");
static_assert(offsetof(Block, message) == 368, "fg_host::Block moved; bump kVersion");
static_assert(sizeof(wchar_t) == 2, "the text fields assume UTF-16");

struct Setup {
	uint32_t generator = 0;
	uint32_t width = 0, height = 0, format = 0;
	uint32_t adapter_luid_low = 0;
	int32_t adapter_luid_high = 0;
};

bool read_setup(const Block &block, Setup *out, const wchar_t **why) noexcept;

struct Request {
	uint64_t seq = 0;
	Command command = Command::None;
	uint32_t generated = 1;
	uint64_t wait_value = 0;
	uint64_t signal_value = 0;
	bool reset = false;
	uint32_t color_space = 0;
	float frame_ms = 16.7f;
	uint32_t window = 0;
};

bool read_request(const volatile Block &block, Request *out) noexcept;

bool format_supported(uint32_t format) noexcept;

}

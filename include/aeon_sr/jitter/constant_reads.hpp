#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace reshade::api {
struct command_list;
}

namespace aeon_sr {

class ConstantReads {
public:
	struct Key {
		uint64_t shader = 0;
		uint32_t slot = 0;
		uint32_t reg = 0;
		bool operator==(const Key &o) const noexcept { return shader == o.shader && slot == o.slot && reg == o.reg; }
	};
	static constexpr uint64_t kFresh = 120u;
	static constexpr uint32_t kAgree = 8u;
	static constexpr uint64_t kGiveUp = 60u;
	static constexpr size_t kMaxKeys = 4096u;

	bool read(const Key &key, uint64_t now, float out[4]);
	bool want(const Key &key);
	void landed(const Key &key, const float value[4], uint64_t now);
	void abandoned(const Key &key);
	void forget_shader(uint64_t shader);
	void clear();
	size_t size();

private:
	struct Hash {
		size_t operator()(const Key &k) const noexcept
		{
			return static_cast<size_t>(k.shader * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(k.slot) << 48) ^ k.reg);
		}
	};
	struct Entry {
		float value[4]{};
		uint64_t read_at = 0;
		uint8_t zeros = 0;
		uint32_t agree = 0;
		bool pending = false;
	};
	std::mutex lock_;
	std::unordered_map<Key, Entry, Hash> entries_;
};

ConstantReads &constant_reads() noexcept;

bool d3d11_vertex_constant(reshade::api::command_list *cmd, uint64_t shader, uint32_t slot, uint32_t reg,
	uint64_t now, float out[4]);
bool d3d10_vertex_constant(reshade::api::command_list *cmd, uint64_t shader, uint32_t slot, uint32_t reg,
	uint64_t now, uint64_t game, float out[4]);
void constant_reads_poll(reshade::api::command_list *cmd, uint64_t now, bool wait = false);
bool d3d11_vertex_position_replaced(reshade::api::command_list *cmd);
void constant_reads_forget(uint64_t native_device);

}

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace reshade::api {
struct command_list;
struct pipeline_layout_param;
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
	bool ask(const Key &key);
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

struct D3D12TableRange {
	uint32_t slot = 0, space = 0, count = 0, offset = 0;
};

struct D3D12RootParam {
	enum class Kind : uint8_t { Other, Constants, View, Table };
	Kind kind = Kind::Other;
	uint32_t slot = 0, space = 0;
	uint32_t count = 0, base = 0;
	std::vector<D3D12TableRange> ranges;
};
using D3D12RootLayout = std::vector<D3D12RootParam>;

void d3d12_note_layout(uint64_t layout, uint32_t count, const reshade::api::pipeline_layout_param *params);
void d3d12_forget_layout(uint64_t layout);
bool d3d12_vertex_tables_seen() noexcept;
void d3d12_forget_layouts();
std::shared_ptr<const D3D12RootLayout> d3d12_layout(uint64_t layout);
D3D12RootLayout d3d12_root_layout(uint32_t count, const reshade::api::pipeline_layout_param *params);

struct D3D12RootArgs {
	static constexpr uint32_t kWords = 64u;
	static constexpr uint32_t kParams = 64u;
	uint64_t layout = 0;
	std::shared_ptr<const D3D12RootLayout> params;
	uint32_t words[kWords]{};
	uint64_t words_set = 0;
	uint64_t view_buffer[kParams]{};
	uint64_t view_offset[kParams]{};
	uint64_t views_set = 0;
	uint64_t table[kParams]{};
	uint64_t tables_set = 0;
	uint64_t epoch = 0;

	void clear() noexcept;
	void use(uint64_t layout);
	void constants(uint64_t layout, uint32_t param, uint32_t first, uint32_t count, const uint32_t *values);
	void view(uint64_t layout, uint32_t param, uint64_t buffer, uint64_t offset);
	void tables(uint64_t layout, uint32_t first, uint32_t count, const uint64_t *handles);

private:
	bool looked = false;
};

struct D3D12Place {
	enum class Kind : uint8_t { None, Value, View, Table };
	Kind kind = Kind::None;
	float value[4]{};
	uint64_t buffer = 0, offset = 0;
	uint64_t table = 0;
	uint32_t index = 0;
};
D3D12Place d3d12_root_place(const D3D12RootArgs &args, uint64_t layout, uint32_t space, uint32_t slot, uint32_t reg);

class D3D12Buffers {
public:
	void note(uint64_t buffer, uint64_t va, uint64_t bytes, const void *cpu);
	void forget(uint64_t buffer);
	bool read(uint64_t buffer, uint64_t offset, float out[4]);
	bool read_va(uint64_t va, float out[4]);
	size_t size();
	void clear();

private:
	struct Entry {
		uint64_t buffer = 0, bytes = 0;
		const uint8_t *cpu = nullptr;
	};
	std::shared_mutex lock_;
	std::map<uint64_t, Entry> by_va_;
	std::unordered_map<uint64_t, uint64_t> va_of_;
};
D3D12Buffers &d3d12_buffers() noexcept;

const void *d3d12_map_cpu_buffer(uint64_t buffer, uint64_t *va, uint64_t *bytes);

class D3D12CbvHeaps {
public:
	static constexpr uint32_t kPage = 256u;
	static constexpr uint32_t kMaxIndex = 1u << 20;
	static constexpr uint32_t kHeaps = 128u;
	static constexpr uint32_t kMaxPages = 8192u;

	D3D12CbvHeaps();
	~D3D12CbvHeaps();
	D3D12CbvHeaps(const D3D12CbvHeaps &) = delete;
	D3D12CbvHeaps &operator=(const D3D12CbvHeaps &) = delete;

	void written(uint64_t heap, uint32_t index, uint64_t va, uint64_t bytes);
	void cleared(uint64_t heap, uint32_t index, uint32_t count);
	void copied(uint64_t dst_heap, uint32_t dst_index, uint64_t src_heap, uint32_t src_index, uint32_t count);
	bool find(uint64_t heap, uint32_t index, uint64_t *va, uint64_t *bytes) const;
	bool any() const noexcept;
	size_t pages() const noexcept;
	void clear();

private:
	struct Page;
	struct Heap;
	Heap *heap_of(uint64_t heap) const;
	Heap *make_heap(uint64_t heap);
	std::atomic<uint64_t> *entry(Heap *h, uint32_t index, bool make);
	Heap *heaps_;
	std::atomic<uint32_t> heap_count_{ 0 };
	std::atomic<uint32_t> page_count_{ 0 };
	std::atomic<bool> any_{ false };
	std::mutex make_lock_;
};
D3D12CbvHeaps &d3d12_cbv_heaps() noexcept;

class D3D12ConstantChecks {
public:
	struct Key {
		uint64_t pipeline = 0;
		uint32_t space = 0, slot = 0, reg = 0;
		bool operator==(const Key &o) const noexcept
		{
			return pipeline == o.pipeline && space == o.space && slot == o.slot && reg == o.reg;
		}
	};
	static constexpr uint32_t kConfirm = 8u;
	static constexpr uint32_t kAgree = ConstantReads::kAgree;
	static constexpr uint64_t kFresh = ConstantReads::kFresh;
	static constexpr size_t kMaxKeys = 4096u;

	void checked(const Key &key, const float *at_draw, const float *at_submit, uint64_t now);
	bool trusted(const Key &key);
	bool agreed(const Key &key, uint64_t now, float out[4]);
	void forget_pipeline(uint64_t pipeline);
	void clear();
	size_t size();
	uint64_t disagreed();

private:
	struct Hash {
		size_t operator()(const Key &k) const noexcept
		{
			return static_cast<size_t>(k.pipeline * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(k.space) << 52) ^
				(static_cast<uint64_t>(k.slot) << 40) ^ k.reg);
		}
	};
	struct Entry {
		float value[4]{};
		uint64_t read_at = 0;
		uint8_t zeros = 0;
		uint32_t agree = 0;
		uint32_t confirmed = 0;
	};
	std::mutex lock_;
	std::unordered_map<Key, Entry, Hash> entries_;
	std::unordered_map<uint64_t, uint32_t> per_pipeline_;
	uint64_t disagreed_ = 0;
};
D3D12ConstantChecks &d3d12_constant_checks() noexcept;

uint8_t constant_zero_lanes(const float v[4], bool *number) noexcept;

}

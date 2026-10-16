#pragma once

#include "aeon_sr/jitter/constant_reads.hpp"
#include "aeon_sr/jitter/shader_position.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace aeon_sr {

class VulkanMemory {
public:
	static constexpr uint64_t kWhole = ~0ull;

	void created(uint64_t device, uint64_t buffer, uint64_t size);
	void bound(uint64_t buffer, uint64_t memory, uint64_t offset);
	void addressed(uint64_t buffer, uint64_t address);
	void destroyed(uint64_t buffer);
	void mapped(uint64_t device, uint64_t memory, const void *host, uint64_t offset, uint64_t size);
	void unmapped(uint64_t memory);
	void freed(uint64_t memory);
	void forget_device(uint64_t device);
	void clear();

	bool read_buffer(uint64_t buffer, uint64_t offset, void *out, size_t n) const;
	bool read_address(uint64_t address, void *out, size_t n) const;
	bool address_known(uint64_t address, uint64_t *left = nullptr) const;
	void describe_address(uint64_t address, char *out, size_t n) const;
	void describe_buffer(uint64_t buffer, uint64_t offset, char *out, size_t n) const;
	size_t addresses() const;

	size_t buffers() const;
	size_t mappings() const;

private:
	struct Mem {
		uint64_t device = 0;
		const uint8_t *host = nullptr;
		uint64_t map_offset = 0, map_size = 0;
		bool mapped = false;
		uint64_t base = 0, extent = 0;
	};
	struct Buf {
		uint64_t device = 0;
		uint64_t size = 0;
		uint64_t memory = 0, offset = 0;
		uint64_t address = 0;
		bool odd = false;
	};
	bool read_memory(const Mem &m, uint64_t offset, void *out, size_t n) const;
	const Mem *memory_of(uint64_t address, uint64_t *offset, uint64_t *left) const;
	void describe_memory(const Mem *m, uint64_t offset, char *out, size_t n) const;
	void erase_memory(std::unordered_map<uint64_t, Mem>::iterator it);

	mutable std::shared_mutex lock_;
	std::unordered_map<uint64_t, Mem> memories_;
	std::unordered_map<uint64_t, Buf> buffers_;
	std::map<uint64_t, uint64_t> by_base_;
	std::map<uint64_t, uint64_t> odd_;
};

struct VulkanBufferRange {
	uint64_t buffer = 0;
	uint64_t offset = 0;
	uint64_t range = 0;
};

class VulkanDescriptors {
public:
	struct TemplateEntry {
		uint32_t binding = 0, element = 0, count = 0;
		uint32_t type = 0;
		size_t offset = 0, stride = 0;
	};
	struct LayoutBinding {
		uint32_t binding = 0, count = 0;
		bool dynamic = false;
	};

	void write(uint64_t set, uint32_t binding, uint32_t element, const VulkanBufferRange &r);
	bool find(uint64_t set, uint32_t binding, uint32_t element, VulkanBufferRange *out) const;
	void copy(uint64_t from, uint32_t from_binding, uint32_t from_element, uint64_t to, uint32_t to_binding,
		uint32_t to_element, uint32_t count);

	void template_made(uint64_t handle, std::vector<TemplateEntry> entries);
	void write_template(uint64_t set, uint64_t handle, const void *data);
	bool template_of(uint64_t handle, std::vector<TemplateEntry> &out) const;

	void set_layout_made(uint64_t handle, uint64_t device, std::vector<LayoutBinding> bindings);
	uint64_t set_layout_device(uint64_t handle) const;
	void pipeline_layout_made(uint64_t handle, std::vector<uint64_t> set_layouts);
	uint64_t set_layout_of(uint64_t pipeline_layout, uint32_t set) const;
	bool set_layout_known(uint64_t pipeline_layout, uint32_t set) const;
	uint32_t dynamic_count(uint64_t pipeline_layout, uint32_t set) const;
	void dynamic_counts(uint64_t pipeline_layout, uint32_t *out, uint32_t n) const;
	uint64_t layout_generation() const noexcept { return layout_generation_.load(std::memory_order_acquire); }
	bool dynamic_index(uint64_t pipeline_layout, uint32_t set, uint32_t binding, uint32_t element, uint32_t *index) const;

	void descriptor_bytes(const void *bytes, size_t n, uint64_t address, uint64_t range);
	bool address_of(const void *bytes, size_t n, uint64_t *address, uint64_t *range) const;
	static constexpr size_t kMaxBytes = 64u;
	static constexpr size_t kKeyBytes = 8u;
	void note_descriptor_size(bool heap, uint32_t size);
	uint32_t descriptor_size(bool heap) const;

	void clear();
	size_t sets() const;

private:
	struct Binding {
		uint32_t binding = 0, element = 0;
		VulkanBufferRange r;
	};
	static constexpr size_t kMaxSets = 1u << 16;
	static constexpr size_t kBytesSlots = 1u << 14, kBytesWays = 4u;
	struct Bytes {
		uint8_t n = 0;
		uint8_t b[kMaxBytes]{};
		uint32_t serial = 0;
		uint64_t address = 0, range = 0;
	};
	mutable std::shared_mutex lock_;
	std::unordered_map<uint64_t, std::vector<Binding>> sets_;
	std::unordered_map<uint64_t, std::vector<TemplateEntry>> templates_;
	struct SetLayout {
		uint64_t device = 0;
		std::vector<LayoutBinding> bindings;
	};
	std::unordered_map<uint64_t, SetLayout> set_layouts_;
	std::unordered_map<uint64_t, std::vector<uint64_t>> pipeline_layouts_;
	std::atomic<uint64_t> layout_generation_{ 1 };
	mutable std::mutex bytes_lock_;
	std::vector<Bytes> bytes_;
	size_t bytes_turn_ = 0;
	uint32_t bytes_serial_ = 0;
	uint32_t heap_size_ = 0, buffer_size_ = 0;
};

struct VulkanHeapMapping {
	struct Entry {
		uint32_t set = 0, first = 0, count = 0;
		uint32_t resources = 0, source = 0;
		uint32_t heap_offset = 0, push_offset = 0, index_stride = 0, array_stride = 0;
	};
	std::vector<Entry> entries;
};

class VulkanCommandState {
public:
	static constexpr uint32_t kPushBytes = 256u;
	static constexpr uint32_t kSets = 16u;
	static constexpr uint32_t kDynamic = 16u;
	static constexpr uint32_t kPushed = 8u;
	static constexpr uint32_t kDescriptorBuffers = 8u;
	static constexpr size_t kMaxPending = 4096u;

	void push_constants(uint32_t offset, uint32_t size, const void *values);
	void push_data(uint32_t offset, uint32_t size, const void *values);
	bool push_constant_bytes(uint32_t offset, uint32_t size, void *out) const;
	bool push_data_bytes(uint32_t offset, uint32_t size, void *out) const;
	void bind_sets(uint64_t layout, uint32_t first, uint32_t count, const uint64_t *sets, uint32_t dynamic_count,
		const uint32_t *dynamic, const VulkanDescriptors &layouts);
	void push_descriptor(uint32_t set, uint32_t binding, const VulkanBufferRange &r);
	void bind_heap(uint64_t address, uint64_t size);
	void bind_descriptor_buffers(uint32_t count, const uint64_t *addresses);
	void set_descriptor_buffer(uint64_t layout, uint32_t set, uint32_t index, uint64_t offset);
	void reset();

	struct Bound {
		uint64_t set = 0;
		uint64_t layout = 0;
		uint32_t dynamic_count = 0;
		uint32_t dynamic[kDynamic]{};
	};
	struct Pushed {
		uint32_t count = 0;
		uint32_t binding[kPushed]{};
		VulkanBufferRange r[kPushed];
	};
	struct DescriptorBufferSet {
		bool bound = false;
		uint64_t layout = 0;
		uint32_t index = 0;
		uint64_t offset = 0;
	};

	Bound sets[kSets];
	Pushed pushed[kSets];
	uint64_t heap = 0, heap_size = 0;
	uint64_t descriptor_buffers[kDescriptorBuffers]{};
	uint32_t descriptor_buffer_count = 0;
	DescriptorBufferSet descriptor_buffer_sets[kSets];

	struct Pending {
		ConstantReads::Key key;
		enum Source : uint8_t { Buffer, Address, HeapDescriptor, BufferDescriptor } source = Buffer;
		uint64_t buffer = 0, offset = 0, address = 0;
		uint32_t descriptor_size = 0;
		uint64_t left = ~0ull;
	};
	std::vector<Pending> pending;

private:
	uint8_t push_[kPushBytes]{};
	uint64_t push_set_[kPushBytes / 64u]{};
	uint8_t data_[kPushBytes]{};
	uint64_t data_set_[kPushBytes / 64u]{};
	uint64_t counts_layout_ = 0, counts_generation_ = 0;
	uint32_t counts_[kSets]{};
};

enum class VulkanReadMiss : uint8_t {
	NoState,
	NotBound,
	NotMapped,
	NoDescriptor,
	NoMapping,
	NotSubmitted,
	Count
};

struct VulkanConstantStats {
	uint32_t hooked = 0, hookable = 0;
	uint32_t push = 0, sets = 0, heap = 0, descriptor_buffer = 0;
	uint32_t missed[static_cast<size_t>(VulkanReadMiss::Count)]{};
	char first_miss[256]{};
	uint32_t buffers = 0, mappings = 0, addresses = 0;
};

class VulkanConstants {
public:
	bool install(void *device, void *get_device_proc) noexcept;
	bool installed() const noexcept { return hooked_.load(std::memory_order_acquire) != 0u; }
	static void each_hook(void (*fn)(void *user, const char *name, const void *target), void *user) noexcept;
	void disable() noexcept;
	void forget_device(uint64_t device) noexcept;
	void (*between_removal_and_disable)(void *user) = nullptr;
	void *between_user = nullptr;

	int perspective(void *cmd, uint64_t pipeline, uint64_t shader, const SpirvWTerms &terms, uint64_t now);

	void submitted(void *cmd, uint64_t now);
	void reset(void *cmd);
	void destroyed(void *cmd);
	void executed_secondary(void *primary, void *secondary);
	void pipeline_destroyed(uint64_t pipeline);

	VulkanConstantStats take_stats() noexcept;

	VulkanMemory &memory() noexcept { return memory_; }
	VulkanDescriptors &descriptors() noexcept { return descriptors_; }
	ConstantReads &reads() noexcept { return reads_; }
	VulkanCommandState *state(void *cmd, bool make = true);
	void heap_mapping(uint64_t pipeline, std::shared_ptr<const VulkanHeapMapping> mapping);
	std::shared_ptr<const VulkanHeapMapping> heap_mapping(uint64_t pipeline) const;
	bool binding_offset(uint64_t set_layout, uint32_t binding, uint64_t *offset) const noexcept;
	void clear() noexcept;

private:
	bool locate(void *cmd, uint64_t pipeline, const SpirvWTerms &terms, uint32_t i, const ConstantReads::Key &key,
		float now_value[4], bool *now_known);
	void miss(VulkanReadMiss why, const char *detail = nullptr) noexcept;
	bool resolve(const VulkanCommandState::Pending &p, float out[4]);

	VulkanMemory memory_;
	VulkanDescriptors descriptors_;
	ConstantReads reads_;
	mutable std::mutex states_lock_;
	std::unordered_map<void *, std::unique_ptr<VulkanCommandState>> states_;
	mutable std::shared_mutex mappings_lock_;
	std::unordered_map<uint64_t, std::shared_ptr<const VulkanHeapMapping>> mappings_;
	std::mutex stats_lock_;
	VulkanConstantStats stats_;
	void *binding_offset_fn_ = nullptr;
	std::atomic<uint32_t> hooked_{ 0 }, hookable_{ 0 };
	std::mutex devices_lock_;
	std::vector<uint64_t> devices_;
	std::mutex install_lock_;
	static uint64_t fresh_epoch() noexcept;
	std::atomic<uint64_t> epoch_{ fresh_epoch() };
};

VulkanConstants &vulkan_constants() noexcept;

}

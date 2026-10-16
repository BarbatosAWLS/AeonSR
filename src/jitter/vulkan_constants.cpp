#include "aeon_sr/jitter/vulkan_constants.hpp"

#include "aeon_sr/core/diagnostics.hpp"

#include <Windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace aeon_sr {

namespace {

template <typename H>
uint64_t hv(H h) noexcept
{
	if constexpr (std::is_pointer_v<H>)
		return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
	else
		return static_cast<uint64_t>(h);
}

template <typename H>
H vk_of(uint64_t v) noexcept
{
	if constexpr (std::is_pointer_v<H>)
		return reinterpret_cast<H>(static_cast<uintptr_t>(v));
	else
		return static_cast<H>(v);
}

bool guarded_copy(void *out, const void *from, size_t n) noexcept
{
	__try {
		std::memcpy(out, from, n);
		return true;
	} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		return false;
	}
}

uint64_t bytes_hash(const void *p, size_t n) noexcept
{
	uint64_t h = 1469598103934665603ull;
	const auto *b = static_cast<const uint8_t *>(p);
	for (size_t i = 0; i < n; ++i) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return h;
}

bool buffer_descriptor(VkDescriptorType t) noexcept
{
	return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
		t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

bool dynamic_descriptor(VkDescriptorType t) noexcept
{
	return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

}

void VulkanMemory::created(uint64_t device, uint64_t buffer, uint64_t size)
{
	if (buffer == 0)
		return;
	const std::unique_lock<std::shared_mutex> lock(lock_);
	const auto old = buffers_.find(buffer);
	if (old != buffers_.end() && old->second.odd)
		odd_.erase(old->second.address);
	Buf &b = buffers_[buffer];
	b = Buf{};
	b.device = device;
	b.size = size;
}

void VulkanMemory::bound(uint64_t buffer, uint64_t memory, uint64_t offset)
{
	if (buffer == 0)
		return;
	const std::unique_lock<std::shared_mutex> lock(lock_);
	const auto it = buffers_.find(buffer);
	if (it == buffers_.end())
		return;
	it->second.memory = memory;
	it->second.offset = offset;
}

void VulkanMemory::addressed(uint64_t buffer, uint64_t address)
{
	if (buffer == 0 || address == 0)
		return;
	const std::unique_lock<std::shared_mutex> lock(lock_);
	const auto it = buffers_.find(buffer);
	if (it == buffers_.end())
		return;
	Buf &b = it->second;
	if (b.odd)
		odd_.erase(b.address);
	b.address = address;
	b.odd = false;
	if (b.memory == 0 || address < b.offset) {
		b.odd = true;
		odd_[address] = buffer;
		return;
	}
	Mem &m = memories_[b.memory];
	if (m.device == 0)
		m.device = b.device;
	const uint64_t base = address - b.offset;
	if (m.base == 0) {
		m.base = base;
		by_base_[base] = b.memory;
	} else if (m.base != base) {
		b.odd = true;
		odd_[address] = buffer;
		return;
	}
	m.extent = std::max(m.extent, b.offset + b.size);
}

void VulkanMemory::destroyed(uint64_t buffer)
{
	const std::unique_lock<std::shared_mutex> lock(lock_);
	const auto it = buffers_.find(buffer);
	if (it == buffers_.end())
		return;
	if (it->second.odd) {
		const auto a = odd_.find(it->second.address);
		if (a != odd_.end() && a->second == buffer)
			odd_.erase(a);
	}
	buffers_.erase(it);
}

void VulkanMemory::mapped(uint64_t device, uint64_t memory, const void *host, uint64_t offset, uint64_t size)
{
	if (memory == 0 || host == nullptr)
		return;
	const std::unique_lock<std::shared_mutex> lock(lock_);
	Mem &m = memories_[memory];
	m.device = device;
	m.host = static_cast<const uint8_t *>(host);
	m.map_offset = offset;
	m.map_size = size;
	m.mapped = true;
}

void VulkanMemory::unmapped(uint64_t memory)
{
	const std::unique_lock<std::shared_mutex> lock(lock_);
	const auto it = memories_.find(memory);
	if (it == memories_.end())
		return;
	it->second.mapped = false;
	it->second.host = nullptr;
}

void VulkanMemory::erase_memory(std::unordered_map<uint64_t, Mem>::iterator it)
{
	if (it->second.base != 0) {
		const auto b = by_base_.find(it->second.base);
		if (b != by_base_.end() && b->second == it->first)
			by_base_.erase(b);
	}
	memories_.erase(it);
}

void VulkanMemory::freed(uint64_t memory)
{
	const std::unique_lock<std::shared_mutex> lock(lock_);
	const auto it = memories_.find(memory);
	if (it != memories_.end())
		erase_memory(it);
}

void VulkanMemory::forget_device(uint64_t device)
{
	const std::unique_lock<std::shared_mutex> lock(lock_);
	for (auto it = memories_.begin(); it != memories_.end();) {
		const auto next = std::next(it);
		if (it->second.device == device)
			erase_memory(it);
		it = next;
	}
	for (auto it = buffers_.begin(); it != buffers_.end();) {
		if (it->second.device != device) {
			++it;
			continue;
		}
		if (it->second.odd)
			odd_.erase(it->second.address);
		it = buffers_.erase(it);
	}
}

void VulkanMemory::clear()
{
	const std::unique_lock<std::shared_mutex> lock(lock_);
	memories_.clear();
	buffers_.clear();
	by_base_.clear();
	odd_.clear();
}

bool VulkanMemory::read_memory(const Mem &m, uint64_t offset, void *out, size_t n) const
{
	if (!m.mapped || m.host == nullptr || offset < m.map_offset)
		return false;
	const uint64_t at = offset - m.map_offset;
	if (m.map_size != kWhole && (at > m.map_size || n > m.map_size - at))
		return false;
	return guarded_copy(out, m.host + at, n);
}

const VulkanMemory::Mem *VulkanMemory::memory_of(uint64_t address, uint64_t *offset, uint64_t *left) const
{
	auto o = odd_.upper_bound(address);
	if (o != odd_.begin()) {
		--o;
		const auto b = buffers_.find(o->second);
		if (b != buffers_.end() && address - o->first < b->second.size && b->second.memory != 0) {
			const auto m = memories_.find(b->second.memory);
			if (m != memories_.end()) {
				*offset = b->second.offset + (address - o->first);
				*left = b->second.size - (address - o->first);
				return &m->second;
			}
		}
	}
	auto it = by_base_.upper_bound(address);
	if (it == by_base_.begin())
		return nullptr;
	--it;
	const auto m = memories_.find(it->second);
	if (m == memories_.end() || address - it->first >= m->second.extent)
		return nullptr;
	*offset = address - it->first;
	*left = m->second.extent - *offset;
	return &m->second;
}

bool VulkanMemory::read_buffer(uint64_t buffer, uint64_t offset, void *out, size_t n) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto b = buffers_.find(buffer);
	if (b == buffers_.end() || b->second.memory == 0 || offset > b->second.size || n > b->second.size - offset)
		return false;
	const auto m = memories_.find(b->second.memory);
	return m != memories_.end() && read_memory(m->second, b->second.offset + offset, out, n);
}

bool VulkanMemory::read_address(uint64_t address, void *out, size_t n) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	uint64_t offset = 0, room = 0;
	const Mem *const m = memory_of(address, &offset, &room);
	return m != nullptr && n <= room && read_memory(*m, offset, out, n);
}

bool VulkanMemory::address_known(uint64_t address, uint64_t *left) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	uint64_t offset = 0, room = 0;
	const Mem *const m = memory_of(address, &offset, &room);
	if (m == nullptr)
		return false;
	if (left != nullptr)
		*left = room;
	return true;
}

void VulkanMemory::describe_memory(const Mem *m, uint64_t offset, char *out, size_t n) const
{
	if (m == nullptr) {
		std::snprintf(out, n, "in no allocation a buffer with an address was bound to");
	} else if (!m->mapped) {
		std::snprintf(out, n, "%llu bytes into an allocation the game has not mapped",
			static_cast<unsigned long long>(offset));
	} else {
		std::snprintf(out, n, "%llu bytes into an allocation mapped from %llu, %llu long",
			static_cast<unsigned long long>(offset), static_cast<unsigned long long>(m->map_offset),
			static_cast<unsigned long long>(m->map_size));
	}
}

void VulkanMemory::describe_address(uint64_t address, char *out, size_t n) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	uint64_t offset = 0, room = 0;
	const Mem *const m = memory_of(address, &offset, &room);
	char at[160];
	describe_memory(m, offset, at, sizeof(at));
	std::snprintf(out, n, "address 0x%llx %s (%zu allocations with an address, %zu buffers of their own)",
		static_cast<unsigned long long>(address), at, by_base_.size(), odd_.size());
}

void VulkanMemory::describe_buffer(uint64_t buffer, uint64_t offset, char *out, size_t n) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto it = buffers_.find(buffer);
	if (it == buffers_.end()) {
		std::snprintf(out, n, "a buffer the hooks never saw made");
		return;
	}
	if (it->second.memory == 0) {
		std::snprintf(out, n, "a buffer of %llu bytes with no memory bound",
			static_cast<unsigned long long>(it->second.size));
		return;
	}
	const auto m = memories_.find(it->second.memory);
	char at[160];
	describe_memory(m != memories_.end() ? &m->second : nullptr, it->second.offset + offset, at, sizeof(at));
	std::snprintf(out, n, "%llu bytes into a buffer of %llu, %s", static_cast<unsigned long long>(offset),
		static_cast<unsigned long long>(it->second.size), at);
}

size_t VulkanMemory::addresses() const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	return by_base_.size() + odd_.size();
}

size_t VulkanMemory::buffers() const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	return buffers_.size();
}

size_t VulkanMemory::mappings() const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	size_t n = 0;
	for (const auto &m : memories_)
		n += m.second.mapped ? 1u : 0u;
	return n;
}

void VulkanDescriptors::write(uint64_t set, uint32_t binding, uint32_t element, const VulkanBufferRange &r)
{
	if (set == 0)
		return;
	const std::unique_lock<std::shared_mutex> lock(lock_);
	if (sets_.size() >= kMaxSets && sets_.find(set) == sets_.end())
		sets_.clear();
	std::vector<Binding> &v = sets_[set];
	for (Binding &b : v) {
		if (b.binding == binding && b.element == element) {
			b.r = r;
			return;
		}
	}
	Binding b;
	b.binding = binding;
	b.element = element;
	b.r = r;
	v.push_back(b);
}

bool VulkanDescriptors::find(uint64_t set, uint32_t binding, uint32_t element, VulkanBufferRange *out) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto it = sets_.find(set);
	if (it == sets_.end())
		return false;
	for (const Binding &b : it->second) {
		if (b.binding == binding && b.element == element) {
			*out = b.r;
			return true;
		}
	}
	return false;
}

void VulkanDescriptors::copy(uint64_t from, uint32_t from_binding, uint32_t from_element, uint64_t to,
	uint32_t to_binding, uint32_t to_element, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i) {
		VulkanBufferRange r;
		if (find(from, from_binding, from_element + i, &r))
			write(to, to_binding, to_element + i, r);
	}
}

void VulkanDescriptors::template_made(uint64_t handle, std::vector<TemplateEntry> entries)
{
	if (handle == 0)
		return;
	const std::unique_lock<std::shared_mutex> lock(lock_);
	templates_[handle] = std::move(entries);
}

void VulkanDescriptors::write_template(uint64_t set, uint64_t handle, const void *data)
{
	if (set == 0 || data == nullptr)
		return;
	const std::unique_lock<std::shared_mutex> lock(lock_);
	const auto t = templates_.find(handle);
	if (t == templates_.end() || t->second.empty())
		return;
	if (sets_.size() >= kMaxSets && sets_.find(set) == sets_.end())
		sets_.clear();
	std::vector<Binding> &v = sets_[set];
	for (const TemplateEntry &e : t->second) {
		for (uint32_t k = 0; k < e.count; ++k) {
			struct {
				uint64_t buffer, offset, range;
			} b;
			std::memcpy(&b, static_cast<const uint8_t *>(data) + e.offset + k * e.stride, sizeof(b));
			Binding *at = nullptr;
			for (Binding &x : v) {
				if (x.binding == e.binding && x.element == e.element + k) {
					at = &x;
					break;
				}
			}
			if (at == nullptr) {
				v.push_back(Binding{});
				at = &v.back();
				at->binding = e.binding;
				at->element = e.element + k;
			}
			at->r.buffer = b.buffer;
			at->r.offset = b.offset;
			at->r.range = b.range;
		}
	}
}

bool VulkanDescriptors::template_of(uint64_t handle, std::vector<TemplateEntry> &out) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto it = templates_.find(handle);
	if (it == templates_.end())
		return false;
	out = it->second;
	return true;
}

void VulkanDescriptors::set_layout_made(uint64_t handle, uint64_t device, std::vector<LayoutBinding> bindings)
{
	if (handle == 0)
		return;
	std::sort(bindings.begin(), bindings.end(),
		[](const LayoutBinding &a, const LayoutBinding &b) { return a.binding < b.binding; });
	const std::unique_lock<std::shared_mutex> lock(lock_);
	SetLayout &l = set_layouts_[handle];
	l.device = device;
	l.bindings = std::move(bindings);
	layout_generation_.fetch_add(1u, std::memory_order_acq_rel);
}

uint64_t VulkanDescriptors::set_layout_device(uint64_t handle) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto it = set_layouts_.find(handle);
	return it != set_layouts_.end() ? it->second.device : 0u;
}

void VulkanDescriptors::pipeline_layout_made(uint64_t handle, std::vector<uint64_t> set_layouts)
{
	if (handle == 0)
		return;
	const std::unique_lock<std::shared_mutex> lock(lock_);
	pipeline_layouts_[handle] = std::move(set_layouts);
	layout_generation_.fetch_add(1u, std::memory_order_acq_rel);
}

uint64_t VulkanDescriptors::set_layout_of(uint64_t pipeline_layout, uint32_t set) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto it = pipeline_layouts_.find(pipeline_layout);
	return it != pipeline_layouts_.end() && set < it->second.size() ? it->second[set] : 0u;
}

bool VulkanDescriptors::set_layout_known(uint64_t pipeline_layout, uint32_t set) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto pl = pipeline_layouts_.find(pipeline_layout);
	return pl != pipeline_layouts_.end() && set < pl->second.size() && set_layouts_.count(pl->second[set]) != 0u;
}

uint32_t VulkanDescriptors::dynamic_count(uint64_t pipeline_layout, uint32_t set) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto pl = pipeline_layouts_.find(pipeline_layout);
	if (pl == pipeline_layouts_.end() || set >= pl->second.size())
		return 0u;
	const auto sl = set_layouts_.find(pl->second[set]);
	if (sl == set_layouts_.end())
		return 0u;
	uint32_t n = 0;
	for (const LayoutBinding &b : sl->second.bindings)
		n += b.dynamic ? b.count : 0u;
	return n;
}

void VulkanDescriptors::dynamic_counts(uint64_t pipeline_layout, uint32_t *out, uint32_t n) const
{
	for (uint32_t s = 0; s < n; ++s)
		out[s] = 0u;
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto pl = pipeline_layouts_.find(pipeline_layout);
	if (pl == pipeline_layouts_.end())
		return;
	for (uint32_t s = 0; s < n && s < pl->second.size(); ++s) {
		const auto sl = set_layouts_.find(pl->second[s]);
		if (sl == set_layouts_.end())
			continue;
		for (const LayoutBinding &b : sl->second.bindings)
			out[s] += b.dynamic ? b.count : 0u;
	}
}

bool VulkanDescriptors::dynamic_index(uint64_t pipeline_layout, uint32_t set, uint32_t binding, uint32_t element,
	uint32_t *index) const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto pl = pipeline_layouts_.find(pipeline_layout);
	if (pl == pipeline_layouts_.end() || set >= pl->second.size())
		return false;
	const auto sl = set_layouts_.find(pl->second[set]);
	if (sl == set_layouts_.end())
		return false;
	uint32_t n = 0;
	for (const LayoutBinding &b : sl->second.bindings) {
		if (!b.dynamic)
			continue;
		if (b.binding == binding) {
			if (element >= b.count)
				return false;
			*index = n + element;
			return true;
		}
		n += b.count;
	}
	return false;
}

void VulkanDescriptors::descriptor_bytes(const void *bytes, size_t n, uint64_t address, uint64_t range)
{
	if (bytes == nullptr || n == 0)
		return;
	const size_t kept = n < kMaxBytes ? n : kMaxBytes;
	const size_t key = kept < kKeyBytes ? kept : kKeyBytes;
	const size_t first = static_cast<size_t>(bytes_hash(bytes, key) % (kBytesSlots / kBytesWays)) * kBytesWays;
	const std::lock_guard<std::mutex> lock(bytes_lock_);
	if (bytes_.empty())
		bytes_.resize(kBytesSlots);
	size_t at = first + (bytes_turn_++ % kBytesWays);
	for (size_t k = 0; k < kBytesWays; ++k) {
		const Bytes &e = bytes_[first + k];
		if (e.n != 0 && ((e.address == address && e.range == range) ||
				(e.n == kept && std::memcmp(e.b, bytes, kept) == 0))) {
			at = first + k;
			break;
		}
		if (e.n == 0)
			at = first + k;
	}
	Bytes &e = bytes_[at];
	e.n = static_cast<uint8_t>(kept);
	std::memcpy(e.b, bytes, kept);
	e.serial = ++bytes_serial_;
	e.address = address;
	e.range = range;
}

bool VulkanDescriptors::address_of(const void *bytes, size_t n, uint64_t *address, uint64_t *range) const
{
	if (bytes == nullptr || n == 0 || n > kMaxBytes)
		return false;
	const size_t key = n < kKeyBytes ? n : kKeyBytes;
	const size_t first = static_cast<size_t>(bytes_hash(bytes, key) % (kBytesSlots / kBytesWays)) * kBytesWays;
	const std::lock_guard<std::mutex> lock(bytes_lock_);
	if (bytes_.empty())
		return false;
	const Bytes *best = nullptr;
	for (size_t k = 0; k < kBytesWays; ++k) {
		const Bytes &e = bytes_[first + k];
		if (e.n >= n && std::memcmp(e.b, bytes, n) == 0 && (best == nullptr || e.serial - best->serial < 0x80000000u))
			best = &e;
	}
	if (best == nullptr)
		return false;
	*address = best->address;
	*range = best->range;
	return true;
}

void VulkanDescriptors::note_descriptor_size(bool heap, uint32_t size)
{
	const std::lock_guard<std::mutex> lock(bytes_lock_);
	(heap ? heap_size_ : buffer_size_) = size;
}

uint32_t VulkanDescriptors::descriptor_size(bool heap) const
{
	const std::lock_guard<std::mutex> lock(bytes_lock_);
	return heap ? heap_size_ : buffer_size_;
}

void VulkanDescriptors::clear()
{
	{
		const std::unique_lock<std::shared_mutex> lock(lock_);
		sets_.clear();
		templates_.clear();
		set_layouts_.clear();
		pipeline_layouts_.clear();
		layout_generation_.fetch_add(1u, std::memory_order_acq_rel);
	}
	const std::lock_guard<std::mutex> lock(bytes_lock_);
	bytes_.clear();
	heap_size_ = buffer_size_ = 0;
}

size_t VulkanDescriptors::sets() const
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	return sets_.size();
}

namespace {

void set_bytes(uint8_t *bytes, uint64_t *mask, uint32_t offset, uint32_t size, const void *values)
{
	constexpr uint32_t n = VulkanCommandState::kPushBytes;
	if (values == nullptr || offset >= n)
		return;
	const uint32_t end = size > n - offset ? n : offset + size;
	std::memcpy(bytes + offset, values, end - offset);
	for (uint32_t i = offset; i < end; ++i)
		mask[i / 64u] |= 1ull << (i % 64u);
}

bool get_bytes(const uint8_t *bytes, const uint64_t *mask, uint32_t offset, uint32_t size, void *out)
{
	constexpr uint32_t n = VulkanCommandState::kPushBytes;
	if (offset > n || size > n - offset)
		return false;
	for (uint32_t i = offset; i < offset + size; ++i)
		if ((mask[i / 64u] & (1ull << (i % 64u))) == 0u)
			return false;
	std::memcpy(out, bytes + offset, size);
	return true;
}

}

void VulkanCommandState::push_constants(uint32_t offset, uint32_t size, const void *values)
{
	set_bytes(push_, push_set_, offset, size, values);
}

void VulkanCommandState::push_data(uint32_t offset, uint32_t size, const void *values)
{
	set_bytes(data_, data_set_, offset, size, values);
}

bool VulkanCommandState::push_constant_bytes(uint32_t offset, uint32_t size, void *out) const
{
	return get_bytes(push_, push_set_, offset, size, out);
}

bool VulkanCommandState::push_data_bytes(uint32_t offset, uint32_t size, void *out) const
{
	return get_bytes(data_, data_set_, offset, size, out);
}

void VulkanCommandState::bind_sets(uint64_t layout, uint32_t first, uint32_t count, const uint64_t *sets_in,
	uint32_t dynamic_count, const uint32_t *dynamic, const VulkanDescriptors &layouts)
{
	const uint64_t generation = layouts.layout_generation();
	if (layout != counts_layout_ || generation != counts_generation_) {
		layouts.dynamic_counts(layout, counts_, kSets);
		counts_layout_ = layout;
		counts_generation_ = generation;
	}
	uint32_t used = 0;
	for (uint32_t k = 0; k < count && sets_in != nullptr; ++k) {
		const uint32_t s = first + k;
		const uint32_t n = s < kSets ? counts_[s] : layouts.dynamic_count(layout, s);
		if (s < kSets) {
			Bound &b = sets[s];
			b.set = sets_in[k];
			b.layout = layout;
			b.dynamic_count = 0;
			if (n > kDynamic || used + n > dynamic_count || (n != 0u && dynamic == nullptr)) {
				b.set = 0;
			} else {
				b.dynamic_count = n;
				for (uint32_t i = 0; i < n; ++i)
					b.dynamic[i] = dynamic[used + i];
			}
			pushed[s].count = 0;
		}
		used += n;
	}
}

void VulkanCommandState::push_descriptor(uint32_t set, uint32_t binding, const VulkanBufferRange &r)
{
	if (set >= kSets)
		return;
	Pushed &p = pushed[set];
	for (uint32_t i = 0; i < p.count; ++i) {
		if (p.binding[i] == binding) {
			p.r[i] = r;
			return;
		}
	}
	if (p.count == kPushed)
		return;
	p.binding[p.count] = binding;
	p.r[p.count] = r;
	++p.count;
	sets[set] = Bound{};
}

void VulkanCommandState::bind_heap(uint64_t address, uint64_t size)
{
	heap = address;
	heap_size = size;
}

void VulkanCommandState::bind_descriptor_buffers(uint32_t count, const uint64_t *addresses)
{
	descriptor_buffer_count = count < kDescriptorBuffers ? count : kDescriptorBuffers;
	for (uint32_t i = 0; i < descriptor_buffer_count; ++i)
		descriptor_buffers[i] = addresses[i];
}

void VulkanCommandState::set_descriptor_buffer(uint64_t layout, uint32_t set, uint32_t index, uint64_t offset)
{
	if (set >= kSets)
		return;
	DescriptorBufferSet &d = descriptor_buffer_sets[set];
	d.bound = true;
	d.layout = layout;
	d.index = index;
	d.offset = offset;
}

void VulkanCommandState::reset()
{
	for (Bound &b : sets)
		b = Bound{};
	for (Pushed &p : pushed)
		p = Pushed{};
	for (DescriptorBufferSet &d : descriptor_buffer_sets)
		d = DescriptorBufferSet{};
	heap = heap_size = 0;
	descriptor_buffer_count = 0;
	std::memset(push_set_, 0, sizeof(push_set_));
	std::memset(data_set_, 0, sizeof(data_set_));
}

namespace {

struct StateCache {
	const VulkanConstants *owner = nullptr;
	void *cmd = nullptr;
	VulkanCommandState *state = nullptr;
	uint64_t epoch = 0;
};
thread_local StateCache t_state;

}

uint64_t VulkanConstants::fresh_epoch() noexcept
{
	static std::atomic<uint64_t> next{ 1 };
	return next.fetch_add(1ull << 32, std::memory_order_relaxed);
}

VulkanCommandState *VulkanConstants::state(void *cmd, bool make)
{
	if (cmd == nullptr)
		return nullptr;
	const uint64_t epoch = epoch_.load(std::memory_order_acquire);
	if (t_state.owner == this && t_state.cmd == cmd && t_state.epoch == epoch)
		return t_state.state;
	VulkanCommandState *p = nullptr;
	{
		const std::lock_guard<std::mutex> lock(states_lock_);
		const auto it = states_.find(cmd);
		if (it != states_.end()) {
			p = it->second.get();
		} else if (make) {
			auto s = std::make_unique<VulkanCommandState>();
			p = s.get();
			states_.emplace(cmd, std::move(s));
		}
	}
	if (p != nullptr)
		t_state = StateCache{ this, cmd, p, epoch };
	return p;
}

void VulkanConstants::heap_mapping(uint64_t pipeline, std::shared_ptr<const VulkanHeapMapping> mapping)
{
	if (pipeline == 0)
		return;
	const std::unique_lock<std::shared_mutex> lock(mappings_lock_);
	if (mapping)
		mappings_[pipeline] = std::move(mapping);
	else
		mappings_.erase(pipeline);
}

std::shared_ptr<const VulkanHeapMapping> VulkanConstants::heap_mapping(uint64_t pipeline) const
{
	const std::shared_lock<std::shared_mutex> lock(mappings_lock_);
	const auto it = mappings_.find(pipeline);
	return it != mappings_.end() ? it->second : nullptr;
}

void VulkanConstants::pipeline_destroyed(uint64_t pipeline)
{
	heap_mapping(pipeline, nullptr);
}

void VulkanConstants::miss(VulkanReadMiss why, const char *detail) noexcept
{
	const std::lock_guard<std::mutex> lock(stats_lock_);
	++stats_.missed[static_cast<size_t>(why)];
	if (detail != nullptr && stats_.first_miss[0] == '\0')
		std::snprintf(stats_.first_miss, sizeof(stats_.first_miss), "%s", detail);
}

VulkanConstantStats VulkanConstants::take_stats() noexcept
{
	const std::lock_guard<std::mutex> lock(stats_lock_);
	VulkanConstantStats s = stats_;
	s.hooked = hooked_.load(std::memory_order_acquire);
	s.hookable = hookable_.load(std::memory_order_acquire);
	s.buffers = static_cast<uint32_t>(memory_.buffers());
	s.mappings = static_cast<uint32_t>(memory_.mappings());
	s.addresses = static_cast<uint32_t>(memory_.addresses());
	stats_ = VulkanConstantStats{};
	return s;
}

bool VulkanConstants::binding_offset(uint64_t set_layout, uint32_t binding, uint64_t *offset) const noexcept
{
	const auto fn = reinterpret_cast<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(binding_offset_fn_);
	const uint64_t device = descriptors_.set_layout_device(set_layout);
	if (fn == nullptr || device == 0 || set_layout == 0)
		return false;
	VkDeviceSize o = 0;
	fn(vk_of<VkDevice>(device), vk_of<VkDescriptorSetLayout>(set_layout), binding, &o);
	*offset = o;
	return true;
}

void VulkanConstants::clear() noexcept
{
	memory_.clear();
	descriptors_.clear();
	reads_.clear();
	{
		const std::lock_guard<std::mutex> lock(states_lock_);
		states_.clear();
		epoch_.fetch_add(1u, std::memory_order_acq_rel);
	}
	const std::unique_lock<std::shared_mutex> lock(mappings_lock_);
	mappings_.clear();
}

void VulkanConstants::forget_device(uint64_t device) noexcept
{
	memory_.forget_device(device);
	const std::lock_guard<std::mutex> installing(install_lock_);
	bool last = false;
	{
		const std::lock_guard<std::mutex> lock(devices_lock_);
		const auto it = std::find(devices_.begin(), devices_.end(), device);
		if (it != devices_.end()) {
			devices_.erase(it);
			last = devices_.empty();
		}
	}
	if (last && between_removal_and_disable != nullptr)
		between_removal_and_disable(between_user);
	if (last)
		disable();
}

bool VulkanConstants::locate(void *cmd, uint64_t pipeline, const SpirvWTerms &terms, uint32_t i,
	const ConstantReads::Key &key, float now_value[4], bool *now_known)
{
	VulkanCommandState *const s = state(cmd, false);
	if (s == nullptr) {
		miss(VulkanReadMiss::NoState);
		return false;
	}
	const uint32_t reg_offset = key.reg * 16u;
	float *const value = now_value;
	const auto queue = [this, s](const VulkanCommandState::Pending &q) {
		const std::lock_guard<std::mutex> lock(states_lock_);
		if (s->pending.size() >= VulkanCommandState::kMaxPending)
			return false;
		s->pending.push_back(q);
		return true;
	};
	if (terms.kind[i] == SpirvWTerms::Push) {
		const uint32_t lane = terms.offset[i] & ~3u;
		float probe = 0.0f;
		if (!s->push_constant_bytes(lane, 4u, &probe)) {
			miss(VulkanReadMiss::NotBound);
			return false;
		}
		for (uint32_t k = 0; k < 4u; ++k)
			(void)s->push_constant_bytes(reg_offset + 4u * k, 4u, &value[k]);
		*now_known = true;
		const std::lock_guard<std::mutex> lock(stats_lock_);
		++stats_.push;
		return true;
	}
	const uint32_t set = terms.set[i], binding = terms.binding[i];
	VulkanCommandState::Pending p;
	p.key = key;
	if (const std::shared_ptr<const VulkanHeapMapping> map = heap_mapping(pipeline)) {
		const VulkanHeapMapping::Entry *e = nullptr;
		for (const VulkanHeapMapping::Entry &m : map->entries) {
			if (m.set == set && binding >= m.first && binding - m.first < m.count &&
				(m.resources & (VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT |
					VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT)) != 0u) {
				e = &m;
				break;
			}
		}
		if (e == nullptr) {
			miss(VulkanReadMiss::NoMapping);
			return false;
		}
		const uint32_t shader_index = binding - e->first;
		switch (e->source) {
		case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT:
		case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT: {
			uint32_t index = 0;
			if (e->source == VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT &&
				!s->push_data_bytes(e->push_offset, 4u, &index)) {
				miss(VulkanReadMiss::NotBound);
				return false;
			}
			if (s->heap == 0) {
				miss(VulkanReadMiss::NotBound);
				return false;
			}
			p.source = VulkanCommandState::Pending::HeapDescriptor;
			p.address = s->heap + e->heap_offset + static_cast<uint64_t>(index) * e->index_stride +
				static_cast<uint64_t>(shader_index) * e->array_stride;
			p.offset = reg_offset;
			p.descriptor_size = descriptors_.descriptor_size(true);
			if (p.descriptor_size > VulkanDescriptors::kMaxBytes)
				p.descriptor_size = 0u;
			if (e->array_stride != 0u && e->array_stride <= VulkanDescriptors::kMaxBytes &&
				(p.descriptor_size == 0u || e->array_stride < p.descriptor_size))
				p.descriptor_size = e->array_stride;
			if (p.descriptor_size == 0u) {
				char why[160];
				std::snprintf(why, sizeof(why),
					"the descriptor's size not known: the writes gave %u bytes, the mapping an array stride of %u",
					descriptors_.descriptor_size(true), e->array_stride);
				miss(VulkanReadMiss::NoDescriptor, why);
				return false;
			}
			break;
		}
		case VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT: {
			uint64_t address = 0;
			if (!s->push_data_bytes(e->push_offset, 8u, &address) || address == 0) {
				miss(VulkanReadMiss::NotBound);
				return false;
			}
			p.source = VulkanCommandState::Pending::Address;
			p.address = address + reg_offset;
			break;
		}
		case VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_DATA_EXT: {
			const uint32_t lane = e->push_offset + (terms.offset[i] & ~3u);
			float probe = 0.0f;
			if (!s->push_data_bytes(lane, 4u, &probe)) {
				miss(VulkanReadMiss::NotBound);
				return false;
			}
			for (uint32_t k = 0; k < 4u; ++k)
				(void)s->push_data_bytes(e->push_offset + reg_offset + 4u * k, 4u, &value[k]);
			*now_known = true;
			const std::lock_guard<std::mutex> lock(stats_lock_);
			++stats_.heap;
			return true;
		}
		default:
			miss(VulkanReadMiss::NoMapping);
			return false;
		}
		return queue(p);
	}
	if (set >= VulkanCommandState::kSets) {
		miss(VulkanReadMiss::NotBound);
		return false;
	}
	const VulkanCommandState::DescriptorBufferSet &db = s->descriptor_buffer_sets[set];
	if (db.bound) {
		uint64_t at = 0;
		const uint64_t layout = descriptors_.set_layout_of(db.layout, set);
		if (db.index >= s->descriptor_buffer_count || !binding_offset(layout, binding, &at)) {
			miss(VulkanReadMiss::NotBound);
			return false;
		}
		p.source = VulkanCommandState::Pending::BufferDescriptor;
		p.address = s->descriptor_buffers[db.index] + db.offset + at;
		p.offset = reg_offset;
		p.descriptor_size = descriptors_.descriptor_size(false);
		if (p.descriptor_size == 0u) {
			miss(VulkanReadMiss::NoDescriptor);
			return false;
		}
		return queue(p);
	}
	VulkanBufferRange r;
	bool found = false;
	const VulkanCommandState::Pushed &pushed = s->pushed[set];
	for (uint32_t k = 0; k < pushed.count && !found; ++k) {
		if (pushed.binding[k] == binding) {
			r = pushed.r[k];
			found = true;
		}
	}
	if (!found) {
		const VulkanCommandState::Bound &b = s->sets[set];
		if (b.set != 0 && descriptors_.set_layout_known(b.layout, set) && descriptors_.find(b.set, binding, 0u, &r)) {
			found = true;
			uint32_t index = 0;
			if (descriptors_.dynamic_index(b.layout, set, binding, 0u, &index)) {
				if (index >= b.dynamic_count) {
					miss(VulkanReadMiss::NotBound);
					return false;
				}
				r.offset += b.dynamic[index];
			}
		}
	}
	if (!found || r.buffer == 0) {
		miss(VulkanReadMiss::NotBound);
		return false;
	}
	p.source = VulkanCommandState::Pending::Buffer;
	p.buffer = r.buffer;
	p.offset = r.offset + reg_offset;
	p.left = r.range == VK_WHOLE_SIZE ? ~0ull : r.range > reg_offset ? r.range - reg_offset : 0u;
	return queue(p);
}

int VulkanConstants::perspective(void *cmd, uint64_t pipeline, uint64_t shader, const SpirvWTerms &terms, uint64_t now)
{
	struct Reader {
		VulkanConstants *self;
		void *cmd;
		uint64_t pipeline, shader, now;
		ConstantReads::Key key[SpirvWTerms::kMax];
		float value[SpirvWTerms::kMax][4];
		bool now_known[SpirvWTerms::kMax];
		uint32_t count;
	} r{ this, cmd, pipeline, shader != 0 ? shader : pipeline, now, {}, {}, {}, 0u };
	return spirv_w_terms_perspective(
		terms,
		[](void *user, const SpirvWTerms &t, uint32_t i, float *out) {
			Reader &rd = *static_cast<Reader *>(user);
			ConstantReads::Key key;
			key.shader = rd.shader;
			key.slot = t.kind[i] == SpirvWTerms::Push ? 0xFFFFFFFFu : (t.set[i] << 16) | (t.binding[i] & 0xFFFFu);
			key.reg = t.offset[i] / 16u;
			uint32_t at = rd.count;
			for (uint32_t k = 0; k < rd.count; ++k)
				if (rd.key[k] == key)
					at = k;
			if (at == rd.count) {
				if (rd.count == SpirvWTerms::kMax)
					return false;
				++rd.count;
				rd.key[at] = key;
				if (rd.self->reads_.ask(key) || t.kind[i] == SpirvWTerms::Push)
					(void)rd.self->locate(rd.cmd, rd.pipeline, t, i, key, rd.value[at], &rd.now_known[at]);
			}
			const uint32_t lane = (t.offset[i] % 16u) / 4u;
			if (rd.now_known[at]) {
				*out = rd.value[at][lane];
				return true;
			}
			if (t.kind[i] == SpirvWTerms::Push)
				return false;
			float v[4] = {};
			if (!rd.self->reads_.read(key, rd.now, v))
				return false;
			*out = v[lane];
			return true;
		},
		&r);
}

bool VulkanConstants::resolve(const VulkanCommandState::Pending &p, float out[4])
{
	uint8_t bytes[16] = {};
	uint64_t address = 0, left = p.left;
	switch (p.source) {
	case VulkanCommandState::Pending::Buffer: {
		const size_t n = left < 16u ? static_cast<size_t>(left) : 16u;
		if (n != 0u && !memory_.read_buffer(p.buffer, p.offset, bytes, n)) {
			char why[256];
			memory_.describe_buffer(p.buffer, p.offset, why, sizeof(why));
			miss(VulkanReadMiss::NotMapped, why);
			return false;
		}
		break;
	}
	case VulkanCommandState::Pending::Address:
		address = p.address;
		break;
	case VulkanCommandState::Pending::HeapDescriptor:
	case VulkanCommandState::Pending::BufferDescriptor: {
		uint8_t descriptor[VulkanDescriptors::kMaxBytes] = {};
		uint64_t range = 0;
		if (p.descriptor_size > sizeof(descriptor)) {
			char why[96];
			std::snprintf(why, sizeof(why), "a descriptor of %u bytes, more than the %zu kept of one", p.descriptor_size,
				sizeof(descriptor));
			miss(VulkanReadMiss::NoDescriptor, why);
			return false;
		}
		if (!memory_.read_address(p.address, descriptor, p.descriptor_size)) {
			char where[200], why[256];
			memory_.describe_address(p.address, where, sizeof(where));
			std::snprintf(why, sizeof(why), "the descriptor's %s", where);
			miss(VulkanReadMiss::NotMapped, why);
			return false;
		}
		if (!descriptors_.address_of(descriptor, p.descriptor_size, &address, &range) || address == 0) {
			char why[128];
			std::snprintf(why, sizeof(why), "descriptor bytes %02x%02x%02x%02x%02x%02x%02x%02x stand for no buffer seen written",
				descriptor[0], descriptor[1], descriptor[2], descriptor[3], descriptor[4], descriptor[5], descriptor[6],
				descriptor[7]);
			miss(VulkanReadMiss::NoDescriptor, why);
			return false;
		}
		left = range == VK_WHOLE_SIZE ? ~0ull : range > p.offset ? range - p.offset : 0u;
		address += p.offset;
		break;
	}
	}
	if (p.source != VulkanCommandState::Pending::Buffer && left != 0u) {
		uint64_t in_buffer = 0;
		const bool known = memory_.address_known(address, &in_buffer);
		const uint64_t n64 = std::min<uint64_t>(std::min<uint64_t>(left, in_buffer), 16u);
		if (!known || n64 == 0u || !memory_.read_address(address, bytes, static_cast<size_t>(n64))) {
			char where[200], why[256];
			memory_.describe_address(address, where, sizeof(where));
			std::snprintf(why, sizeof(why), "the buffer's %s", where);
			miss(VulkanReadMiss::NotMapped, why);
			return false;
		}
	}
	std::memcpy(out, bytes, 16u);
	const std::lock_guard<std::mutex> lock(stats_lock_);
	if (p.source == VulkanCommandState::Pending::Buffer)
		++stats_.sets;
	else if (p.source == VulkanCommandState::Pending::BufferDescriptor)
		++stats_.descriptor_buffer;
	else
		++stats_.heap;
	return true;
}

void VulkanConstants::submitted(void *cmd, uint64_t now)
{
	std::vector<VulkanCommandState::Pending> pending;
	{
		const std::lock_guard<std::mutex> lock(states_lock_);
		const auto it = states_.find(cmd);
		if (it == states_.end() || it->second->pending.empty())
			return;
		pending.swap(it->second->pending);
	}
	for (const VulkanCommandState::Pending &p : pending) {
		float v[4] = {};
		if (resolve(p, v))
			reads_.landed(p.key, v, now);
		else
			reads_.abandoned(p.key);
	}
}

void VulkanConstants::reset(void *cmd)
{
	std::vector<VulkanCommandState::Pending> pending;
	{
		const std::lock_guard<std::mutex> lock(states_lock_);
		const auto it = states_.find(cmd);
		if (it == states_.end())
			return;
		it->second->reset();
		pending.swap(it->second->pending);
	}
	for (const VulkanCommandState::Pending &p : pending) {
		reads_.abandoned(p.key);
		miss(VulkanReadMiss::NotSubmitted);
	}
}

void VulkanConstants::destroyed(void *cmd)
{
	std::unique_ptr<VulkanCommandState> gone;
	{
		const std::lock_guard<std::mutex> lock(states_lock_);
		const auto it = states_.find(cmd);
		if (it == states_.end())
			return;
		gone = std::move(it->second);
		states_.erase(it);
		epoch_.fetch_add(1u, std::memory_order_acq_rel);
	}
	for (const VulkanCommandState::Pending &p : gone->pending) {
		reads_.abandoned(p.key);
		miss(VulkanReadMiss::NotSubmitted);
	}
}

void VulkanConstants::executed_secondary(void *primary, void *secondary)
{
	if (primary == nullptr || secondary == nullptr || primary == secondary)
		return;
	VulkanCommandState *const to = state(primary);
	const std::lock_guard<std::mutex> lock(states_lock_);
	const auto from = states_.find(secondary);
	if (from == states_.end() || to == nullptr)
		return;
	to->pending.insert(to->pending.end(), from->second->pending.begin(), from->second->pending.end());
	from->second->pending.clear();
}

namespace {

enum Call : uint32_t {
	kCreateBuffer,
	kDestroyBuffer,
	kBindBufferMemory,
	kBindBufferMemory2,
	kBindBufferMemory2Khr,
	kMapMemory,
	kMapMemory2,
	kMapMemory2Khr,
	kUnmapMemory,
	kUnmapMemory2,
	kUnmapMemory2Khr,
	kFreeMemory,
	kBufferAddress,
	kBufferAddressKhr,
	kBufferAddressExt,
	kCreateSetLayout,
	kCreatePipelineLayout,
	kUpdateSets,
	kUpdateWithTemplate,
	kUpdateWithTemplateKhr,
	kCreateTemplate,
	kCreateTemplateKhr,
	kBindSets,
	kBindSets2,
	kBindSets2Khr,
	kPushConstants,
	kPushConstants2,
	kPushConstants2Khr,
	kPushData,
	kBindResourceHeap,
	kWriteResourceDescriptors,
	kBindDescriptorBuffers,
	kSetDescriptorBufferOffsets,
	kSetDescriptorBufferOffsets2,
	kGetDescriptor,
	kCreateGraphicsPipelines,
	kCallCount
};

struct Hooked {
	std::atomic<void *> target{ nullptr };
	std::atomic<void *> original{ nullptr };
};
Hooked g_calls[kCallCount];

template <typename Pfn>
Pfn orig(Call c) noexcept
{
	return reinterpret_cast<Pfn>(g_calls[c].original.load(std::memory_order_acquire));
}

VulkanConstants &vc() noexcept
{
	return vulkan_constants();
}

template <Call C>
VkResult VKAPI_CALL create_buffer(VkDevice device, const VkBufferCreateInfo *info, const VkAllocationCallbacks *alloc,
	VkBuffer *buffer)
{
	const VkResult r = orig<PFN_vkCreateBuffer>(C)(device, info, alloc, buffer);
	if (r == VK_SUCCESS && info != nullptr && buffer != nullptr)
		vc().memory().created(hv(device), hv(*buffer), info->size);
	return r;
}

template <Call C>
void VKAPI_CALL destroy_buffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *alloc)
{
	vc().memory().destroyed(hv(buffer));
	orig<PFN_vkDestroyBuffer>(C)(device, buffer, alloc);
}

template <Call C>
VkResult VKAPI_CALL bind_buffer_memory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
	const VkResult r = orig<PFN_vkBindBufferMemory>(C)(device, buffer, memory, offset);
	if (r == VK_SUCCESS)
		vc().memory().bound(hv(buffer), hv(memory), offset);
	return r;
}

template <Call C>
VkResult VKAPI_CALL bind_buffer_memory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo *infos)
{
	const VkResult r = orig<PFN_vkBindBufferMemory2>(C)(device, count, infos);
	if (r == VK_SUCCESS && infos != nullptr)
		for (uint32_t i = 0; i < count; ++i)
			vc().memory().bound(hv(infos[i].buffer), hv(infos[i].memory), infos[i].memoryOffset);
	return r;
}

template <Call C>
VkResult VKAPI_CALL map_memory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
	VkMemoryMapFlags flags, void **data)
{
	const VkResult r = orig<PFN_vkMapMemory>(C)(device, memory, offset, size, flags, data);
	if (r == VK_SUCCESS && data != nullptr && *data != nullptr)
		vc().memory().mapped(hv(device), hv(memory), *data, offset, size == VK_WHOLE_SIZE ? VulkanMemory::kWhole : size);
	return r;
}

template <Call C>
VkResult VKAPI_CALL map_memory2(VkDevice device, const VkMemoryMapInfo *info, void **data)
{
	const VkResult r = orig<PFN_vkMapMemory2>(C)(device, info, data);
	if (r == VK_SUCCESS && info != nullptr && data != nullptr && *data != nullptr)
		vc().memory().mapped(hv(device), hv(info->memory), *data, info->offset,
			info->size == VK_WHOLE_SIZE ? VulkanMemory::kWhole : info->size);
	return r;
}

template <Call C>
void VKAPI_CALL unmap_memory(VkDevice device, VkDeviceMemory memory)
{
	vc().memory().unmapped(hv(memory));
	orig<PFN_vkUnmapMemory>(C)(device, memory);
}

template <Call C>
VkResult VKAPI_CALL unmap_memory2(VkDevice device, const VkMemoryUnmapInfo *info)
{
	if (info != nullptr)
		vc().memory().unmapped(hv(info->memory));
	return orig<PFN_vkUnmapMemory2>(C)(device, info);
}

template <Call C>
void VKAPI_CALL free_memory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *alloc)
{
	vc().memory().freed(hv(memory));
	orig<PFN_vkFreeMemory>(C)(device, memory, alloc);
}

template <Call C>
VkDeviceAddress VKAPI_CALL buffer_address(VkDevice device, const VkBufferDeviceAddressInfo *info)
{
	const VkDeviceAddress a = orig<PFN_vkGetBufferDeviceAddress>(C)(device, info);
	if (info != nullptr)
		vc().memory().addressed(hv(info->buffer), a);
	return a;
}

template <Call C>
VkResult VKAPI_CALL create_set_layout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
	const VkAllocationCallbacks *alloc, VkDescriptorSetLayout *layout)
{
	const VkResult r = orig<PFN_vkCreateDescriptorSetLayout>(C)(device, info, alloc, layout);
	if (r == VK_SUCCESS && info != nullptr && layout != nullptr) {
		std::vector<VulkanDescriptors::LayoutBinding> b;
		for (uint32_t i = 0; i < info->bindingCount && info->pBindings != nullptr; ++i) {
			const VkDescriptorSetLayoutBinding &s = info->pBindings[i];
			if (!buffer_descriptor(s.descriptorType))
				continue;
			VulkanDescriptors::LayoutBinding l;
			l.binding = s.binding;
			l.count = s.descriptorCount;
			l.dynamic = dynamic_descriptor(s.descriptorType);
			b.push_back(l);
		}
		vc().descriptors().set_layout_made(hv(*layout), hv(device), std::move(b));
	}
	return r;
}

template <Call C>
VkResult VKAPI_CALL create_pipeline_layout(VkDevice device, const VkPipelineLayoutCreateInfo *info,
	const VkAllocationCallbacks *alloc, VkPipelineLayout *layout)
{
	const VkResult r = orig<PFN_vkCreatePipelineLayout>(C)(device, info, alloc, layout);
	if (r == VK_SUCCESS && info != nullptr && layout != nullptr) {
		std::vector<uint64_t> sets;
		for (uint32_t i = 0; i < info->setLayoutCount && info->pSetLayouts != nullptr; ++i)
			sets.push_back(hv(info->pSetLayouts[i]));
		vc().descriptors().pipeline_layout_made(hv(*layout), std::move(sets));
	}
	return r;
}

void note_writes(uint32_t count, const VkWriteDescriptorSet *writes)
{
	for (uint32_t i = 0; i < count && writes != nullptr; ++i) {
		const VkWriteDescriptorSet &w = writes[i];
		if (!buffer_descriptor(w.descriptorType) || w.pBufferInfo == nullptr)
			continue;
		for (uint32_t k = 0; k < w.descriptorCount; ++k) {
			VulkanBufferRange r;
			r.buffer = hv(w.pBufferInfo[k].buffer);
			r.offset = w.pBufferInfo[k].offset;
			r.range = w.pBufferInfo[k].range;
			vc().descriptors().write(hv(w.dstSet), w.dstBinding, w.dstArrayElement + k, r);
		}
	}
}

template <Call C>
void VKAPI_CALL update_sets(VkDevice device, uint32_t write_count, const VkWriteDescriptorSet *writes,
	uint32_t copy_count, const VkCopyDescriptorSet *copies)
{
	orig<PFN_vkUpdateDescriptorSets>(C)(device, write_count, writes, copy_count, copies);
	note_writes(write_count, writes);
	for (uint32_t i = 0; i < copy_count && copies != nullptr; ++i) {
		const VkCopyDescriptorSet &c = copies[i];
		vc().descriptors().copy(hv(c.srcSet), c.srcBinding, c.srcArrayElement, hv(c.dstSet), c.dstBinding,
			c.dstArrayElement, c.descriptorCount);
	}
}

template <Call C>
void VKAPI_CALL update_with_template(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl,
	const void *data)
{
	orig<PFN_vkUpdateDescriptorSetWithTemplate>(C)(device, set, tmpl, data);
	if (data != nullptr)
		vc().descriptors().write_template(hv(set), hv(tmpl), data);
}

template <Call C>
VkResult VKAPI_CALL create_template(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *info,
	const VkAllocationCallbacks *alloc, VkDescriptorUpdateTemplate *tmpl)
{
	const VkResult r = orig<PFN_vkCreateDescriptorUpdateTemplate>(C)(device, info, alloc, tmpl);
	if (r == VK_SUCCESS && info != nullptr && tmpl != nullptr &&
		info->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) {
		std::vector<VulkanDescriptors::TemplateEntry> entries;
		for (uint32_t i = 0; i < info->descriptorUpdateEntryCount && info->pDescriptorUpdateEntries != nullptr; ++i) {
			const VkDescriptorUpdateTemplateEntry &s = info->pDescriptorUpdateEntries[i];
			if (!buffer_descriptor(s.descriptorType))
				continue;
			VulkanDescriptors::TemplateEntry e;
			e.binding = s.dstBinding;
			e.element = s.dstArrayElement;
			e.count = s.descriptorCount;
			e.type = static_cast<uint32_t>(s.descriptorType);
			e.offset = s.offset;
			e.stride = s.stride;
			entries.push_back(e);
		}
		vc().descriptors().template_made(hv(*tmpl), std::move(entries));
	}
	return r;
}

bool graphics_stages(VkShaderStageFlags stages) noexcept
{
	return (stages & VK_SHADER_STAGE_VERTEX_BIT) != 0u;
}

void bind_sets_common(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t first, uint32_t count,
	const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *dynamic)
{
	if (sets == nullptr)
		return;
	uint64_t values[VulkanCommandState::kSets]{};
	const uint32_t n = count < VulkanCommandState::kSets ? count : VulkanCommandState::kSets;
	for (uint32_t i = 0; i < n; ++i)
		values[i] = hv(sets[i]);
	if (VulkanCommandState *const s = vc().state(cmd))
		s->bind_sets(hv(layout), first, n, values, dynamic_count, dynamic, vc().descriptors());
}

template <Call C>
void VKAPI_CALL bind_sets(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
	uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *dynamic)
{
	orig<PFN_vkCmdBindDescriptorSets>(C)(cmd, point, layout, first, count, sets, dynamic_count, dynamic);
	if (point == VK_PIPELINE_BIND_POINT_GRAPHICS)
		bind_sets_common(cmd, layout, first, count, sets, dynamic_count, dynamic);
}

template <Call C>
void VKAPI_CALL bind_sets2(VkCommandBuffer cmd, const VkBindDescriptorSetsInfo *info)
{
	orig<PFN_vkCmdBindDescriptorSets2>(C)(cmd, info);
	if (info != nullptr && graphics_stages(info->stageFlags))
		bind_sets_common(cmd, info->layout, info->firstSet, info->descriptorSetCount, info->pDescriptorSets,
			info->dynamicOffsetCount, info->pDynamicOffsets);
}

template <Call C>
void VKAPI_CALL push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
	uint32_t offset, uint32_t size, const void *values)
{
	orig<PFN_vkCmdPushConstants>(C)(cmd, layout, stages, offset, size, values);
	if (graphics_stages(stages))
		if (VulkanCommandState *const s = vc().state(cmd))
			s->push_constants(offset, size, values);
}

template <Call C>
void VKAPI_CALL push_constants2(VkCommandBuffer cmd, const VkPushConstantsInfo *info)
{
	orig<PFN_vkCmdPushConstants2>(C)(cmd, info);
	if (info != nullptr && graphics_stages(info->stageFlags))
		if (VulkanCommandState *const s = vc().state(cmd))
			s->push_constants(info->offset, info->size, info->pValues);
}

template <Call C>
void VKAPI_CALL push_data(VkCommandBuffer cmd, const VkPushDataInfoEXT *info)
{
	orig<PFN_vkCmdPushDataEXT>(C)(cmd, info);
	if (info != nullptr && info->data.address != nullptr && info->data.size <= VulkanCommandState::kPushBytes)
		if (VulkanCommandState *const s = vc().state(cmd))
			s->push_data(info->offset, static_cast<uint32_t>(info->data.size), info->data.address);
}

template <Call C>
void VKAPI_CALL bind_resource_heap(VkCommandBuffer cmd, const VkBindHeapInfoEXT *info)
{
	orig<PFN_vkCmdBindResourceHeapEXT>(C)(cmd, info);
	if (info != nullptr)
		if (VulkanCommandState *const s = vc().state(cmd))
			s->bind_heap(info->heapRange.address, info->heapRange.size);
}

template <Call C>
VkResult VKAPI_CALL write_resource_descriptors(VkDevice device, uint32_t count, const VkResourceDescriptorInfoEXT *infos,
	const VkHostAddressRangeEXT *dsts)
{
	const VkResult r = orig<PFN_vkWriteResourceDescriptorsEXT>(C)(device, count, infos, dsts);
	if (r != VK_SUCCESS || infos == nullptr || dsts == nullptr)
		return r;
	for (uint32_t i = 0; i < count; ++i) {
		const VkResourceDescriptorInfoEXT &d = infos[i];
		if ((d.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && d.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ||
			d.data.pAddressRange == nullptr || dsts[i].address == nullptr)
			continue;
		if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
			vc().descriptors().note_descriptor_size(true, static_cast<uint32_t>(dsts[i].size));
		vc().descriptors().descriptor_bytes(dsts[i].address, dsts[i].size, d.data.pAddressRange->address,
			d.data.pAddressRange->size);
	}
	return r;
}

template <Call C>
void VKAPI_CALL bind_descriptor_buffers(VkCommandBuffer cmd, uint32_t count, const VkDescriptorBufferBindingInfoEXT *infos)
{
	orig<PFN_vkCmdBindDescriptorBuffersEXT>(C)(cmd, count, infos);
	if (infos == nullptr)
		return;
	uint64_t a[VulkanCommandState::kDescriptorBuffers]{};
	const uint32_t n = count < VulkanCommandState::kDescriptorBuffers ? count : VulkanCommandState::kDescriptorBuffers;
	for (uint32_t i = 0; i < n; ++i)
		a[i] = infos[i].address;
	if (VulkanCommandState *const s = vc().state(cmd))
		s->bind_descriptor_buffers(n, a);
}

void set_offsets_common(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t first, uint32_t count,
	const uint32_t *indices, const VkDeviceSize *offsets)
{
	if (indices == nullptr || offsets == nullptr)
		return;
	if (VulkanCommandState *const s = vc().state(cmd))
		for (uint32_t i = 0; i < count; ++i)
			s->set_descriptor_buffer(hv(layout), first + i, indices[i], offsets[i]);
}

template <Call C>
void VKAPI_CALL set_descriptor_buffer_offsets(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
	uint32_t first, uint32_t count, const uint32_t *indices, const VkDeviceSize *offsets)
{
	orig<PFN_vkCmdSetDescriptorBufferOffsetsEXT>(C)(cmd, point, layout, first, count, indices, offsets);
	if (point == VK_PIPELINE_BIND_POINT_GRAPHICS)
		set_offsets_common(cmd, layout, first, count, indices, offsets);
}

template <Call C>
void VKAPI_CALL set_descriptor_buffer_offsets2(VkCommandBuffer cmd, const VkSetDescriptorBufferOffsetsInfoEXT *info)
{
	orig<PFN_vkCmdSetDescriptorBufferOffsets2EXT>(C)(cmd, info);
	if (info != nullptr && graphics_stages(info->stageFlags))
		set_offsets_common(cmd, info->layout, info->firstSet, info->setCount, info->pBufferIndices, info->pOffsets);
}

template <Call C>
void VKAPI_CALL get_descriptor(VkDevice device, const VkDescriptorGetInfoEXT *info, size_t size, void *out)
{
	orig<PFN_vkGetDescriptorEXT>(C)(device, info, size, out);
	if (info == nullptr || out == nullptr)
		return;
	if (info->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && info->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		return;
	const VkDescriptorAddressInfoEXT *a =
		info->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? info->data.pUniformBuffer : info->data.pStorageBuffer;
	if (a == nullptr)
		return;
	if (info->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
		vc().descriptors().note_descriptor_size(false, static_cast<uint32_t>(size));
	vc().descriptors().descriptor_bytes(out, size, a->address, a->range);
}

const void *find_next(const void *chain, VkStructureType type) noexcept
{
	for (auto *p = static_cast<const VkBaseInStructure *>(chain); p != nullptr; p = p->pNext)
		if (p->sType == type)
			return p;
	return nullptr;
}

template <Call C>
VkResult VKAPI_CALL create_graphics_pipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
	const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *alloc, VkPipeline *pipelines)
{
	const VkResult r = orig<PFN_vkCreateGraphicsPipelines>(C)(device, cache, count, infos, alloc, pipelines);
	if (infos == nullptr || pipelines == nullptr || r < VK_SUCCESS)
		return r;
	for (uint32_t i = 0; i < count; ++i) {
		if (pipelines[i] == VK_NULL_HANDLE)
			continue;
		std::shared_ptr<VulkanHeapMapping> m;
		for (uint32_t k = 0; k < infos[i].stageCount && infos[i].pStages != nullptr; ++k) {
			const VkPipelineShaderStageCreateInfo &st = infos[i].pStages[k];
			if (st.stage != VK_SHADER_STAGE_VERTEX_BIT)
				continue;
			const auto *map = static_cast<const VkShaderDescriptorSetAndBindingMappingInfoEXT *>(
				find_next(st.pNext, VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT));
			if (map == nullptr || map->pMappings == nullptr)
				continue;
			m = std::make_shared<VulkanHeapMapping>();
			for (uint32_t j = 0; j < map->mappingCount; ++j) {
				const VkDescriptorSetAndBindingMappingEXT &s = map->pMappings[j];
				VulkanHeapMapping::Entry e;
				e.set = s.descriptorSet;
				e.first = s.firstBinding;
				e.count = s.bindingCount;
				e.resources = s.resourceMask;
				e.source = static_cast<uint32_t>(s.source);
				switch (s.source) {
				case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT:
					e.heap_offset = s.sourceData.constantOffset.heapOffset;
					e.array_stride = s.sourceData.constantOffset.heapArrayStride;
					break;
				case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT:
					e.heap_offset = s.sourceData.pushIndex.heapOffset;
					e.push_offset = s.sourceData.pushIndex.pushOffset;
					e.index_stride = s.sourceData.pushIndex.heapIndexStride;
					e.array_stride = s.sourceData.pushIndex.heapArrayStride;
					break;
				case VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_DATA_EXT:
					e.push_offset = s.sourceData.pushDataOffset;
					break;
				case VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT:
					e.push_offset = s.sourceData.pushAddressOffset;
					break;
				default:
					break;
				}
				m->entries.push_back(e);
			}
		}
		if (!m) {
			const auto *libs = static_cast<const VkPipelineLibraryCreateInfoKHR *>(
				find_next(infos[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR));
			for (uint32_t k = 0; libs != nullptr && libs->pLibraries != nullptr && k < libs->libraryCount; ++k) {
				if (auto lib = vc().heap_mapping(hv(libs->pLibraries[k]))) {
					vc().heap_mapping(hv(pipelines[i]), lib);
					break;
				}
			}
			continue;
		}
		vc().heap_mapping(hv(pipelines[i]), std::move(m));
	}
	return r;
}

struct CallInfo {
	const char *name;
	Call call;
	void *detour;
};

const CallInfo kCalls[] = {
	{ "vkCreateBuffer", kCreateBuffer, reinterpret_cast<void *>(&create_buffer<kCreateBuffer>) },
	{ "vkDestroyBuffer", kDestroyBuffer, reinterpret_cast<void *>(&destroy_buffer<kDestroyBuffer>) },
	{ "vkBindBufferMemory", kBindBufferMemory, reinterpret_cast<void *>(&bind_buffer_memory<kBindBufferMemory>) },
	{ "vkBindBufferMemory2", kBindBufferMemory2, reinterpret_cast<void *>(&bind_buffer_memory2<kBindBufferMemory2>) },
	{ "vkBindBufferMemory2KHR", kBindBufferMemory2Khr, reinterpret_cast<void *>(&bind_buffer_memory2<kBindBufferMemory2Khr>) },
	{ "vkMapMemory", kMapMemory, reinterpret_cast<void *>(&map_memory<kMapMemory>) },
	{ "vkMapMemory2", kMapMemory2, reinterpret_cast<void *>(&map_memory2<kMapMemory2>) },
	{ "vkMapMemory2KHR", kMapMemory2Khr, reinterpret_cast<void *>(&map_memory2<kMapMemory2Khr>) },
	{ "vkUnmapMemory", kUnmapMemory, reinterpret_cast<void *>(&unmap_memory<kUnmapMemory>) },
	{ "vkUnmapMemory2", kUnmapMemory2, reinterpret_cast<void *>(&unmap_memory2<kUnmapMemory2>) },
	{ "vkUnmapMemory2KHR", kUnmapMemory2Khr, reinterpret_cast<void *>(&unmap_memory2<kUnmapMemory2Khr>) },
	{ "vkFreeMemory", kFreeMemory, reinterpret_cast<void *>(&free_memory<kFreeMemory>) },
	{ "vkGetBufferDeviceAddress", kBufferAddress, reinterpret_cast<void *>(&buffer_address<kBufferAddress>) },
	{ "vkGetBufferDeviceAddressKHR", kBufferAddressKhr, reinterpret_cast<void *>(&buffer_address<kBufferAddressKhr>) },
	{ "vkGetBufferDeviceAddressEXT", kBufferAddressExt, reinterpret_cast<void *>(&buffer_address<kBufferAddressExt>) },
	{ "vkCreateDescriptorSetLayout", kCreateSetLayout, reinterpret_cast<void *>(&create_set_layout<kCreateSetLayout>) },
	{ "vkCreatePipelineLayout", kCreatePipelineLayout, reinterpret_cast<void *>(&create_pipeline_layout<kCreatePipelineLayout>) },
	{ "vkUpdateDescriptorSets", kUpdateSets, reinterpret_cast<void *>(&update_sets<kUpdateSets>) },
	{ "vkUpdateDescriptorSetWithTemplate", kUpdateWithTemplate, reinterpret_cast<void *>(&update_with_template<kUpdateWithTemplate>) },
	{ "vkUpdateDescriptorSetWithTemplateKHR", kUpdateWithTemplateKhr, reinterpret_cast<void *>(&update_with_template<kUpdateWithTemplateKhr>) },
	{ "vkCreateDescriptorUpdateTemplate", kCreateTemplate, reinterpret_cast<void *>(&create_template<kCreateTemplate>) },
	{ "vkCreateDescriptorUpdateTemplateKHR", kCreateTemplateKhr, reinterpret_cast<void *>(&create_template<kCreateTemplateKhr>) },
	{ "vkCmdBindDescriptorSets", kBindSets, reinterpret_cast<void *>(&bind_sets<kBindSets>) },
	{ "vkCmdBindDescriptorSets2", kBindSets2, reinterpret_cast<void *>(&bind_sets2<kBindSets2>) },
	{ "vkCmdBindDescriptorSets2KHR", kBindSets2Khr, reinterpret_cast<void *>(&bind_sets2<kBindSets2Khr>) },
	{ "vkCmdPushConstants", kPushConstants, reinterpret_cast<void *>(&push_constants<kPushConstants>) },
	{ "vkCmdPushConstants2", kPushConstants2, reinterpret_cast<void *>(&push_constants2<kPushConstants2>) },
	{ "vkCmdPushConstants2KHR", kPushConstants2Khr, reinterpret_cast<void *>(&push_constants2<kPushConstants2Khr>) },
	{ "vkCmdPushDataEXT", kPushData, reinterpret_cast<void *>(&push_data<kPushData>) },
	{ "vkCmdBindResourceHeapEXT", kBindResourceHeap, reinterpret_cast<void *>(&bind_resource_heap<kBindResourceHeap>) },
	{ "vkWriteResourceDescriptorsEXT", kWriteResourceDescriptors, reinterpret_cast<void *>(&write_resource_descriptors<kWriteResourceDescriptors>) },
	{ "vkCmdBindDescriptorBuffersEXT", kBindDescriptorBuffers, reinterpret_cast<void *>(&bind_descriptor_buffers<kBindDescriptorBuffers>) },
	{ "vkCmdSetDescriptorBufferOffsetsEXT", kSetDescriptorBufferOffsets, reinterpret_cast<void *>(&set_descriptor_buffer_offsets<kSetDescriptorBufferOffsets>) },
	{ "vkCmdSetDescriptorBufferOffsets2EXT", kSetDescriptorBufferOffsets2, reinterpret_cast<void *>(&set_descriptor_buffer_offsets2<kSetDescriptorBufferOffsets2>) },
	{ "vkGetDescriptorEXT", kGetDescriptor, reinterpret_cast<void *>(&get_descriptor<kGetDescriptor>) },
	{ "vkCreateGraphicsPipelines", kCreateGraphicsPipelines, reinterpret_cast<void *>(&create_graphics_pipelines<kCreateGraphicsPipelines>) },
};
static_assert(sizeof(kCalls) / sizeof(kCalls[0]) == kCallCount, "every call has its hook");

}

bool VulkanConstants::install(void *device, void *get_device_proc) noexcept
{
	if (device == nullptr || get_device_proc == nullptr)
		return false;
	auto *const gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_device_proc);
	const VkDevice dev = static_cast<VkDevice>(device);
	const std::lock_guard<std::mutex> installing(install_lock_);
	const MH_STATUS up = MH_Initialize();
	if (up != MH_OK && up != MH_ERROR_ALREADY_INITIALIZED) {
		diag_info("jitter", L"Vulkan constants: the hooking library would not start; quads into the window keep the grid");
		return false;
	}
	uint32_t hooked = 0, hookable = 0;
	void *addresses[kCallCount]{};
	for (const CallInfo &c : kCalls) {
		void *const target = reinterpret_cast<void *>(gdpa(dev, c.name));
		addresses[c.call] = target;
		if (target == nullptr)
			continue;
		++hookable;
		bool alias = false;
		for (const CallInfo &o : kCalls)
			alias = alias || (o.call < c.call && addresses[o.call] == target);
		if (alias) {
			++hooked;
			continue;
		}
		Hooked &h = g_calls[c.call];
		void *const had = h.target.load(std::memory_order_acquire);
		if (had != nullptr && had != target) {
			diag_logf(DiagLevel::Info, "jitter", L"Vulkan constants: %hs is at another address on this device than "
				L"on the first one hooked, and is not hooked for it", c.name);
			continue;
		}
		if (had == nullptr) {
			void *made = nullptr;
			if (MH_CreateHook(target, c.detour, &made) != MH_OK || made == nullptr) {
				diag_logf(DiagLevel::Info, "jitter", L"Vulkan constants: %hs could not be hooked", c.name);
				continue;
			}
			h.original.store(made, std::memory_order_release);
			h.target.store(target, std::memory_order_release);
		}
		const MH_STATUS st = MH_EnableHook(target);
		if (st != MH_OK && st != MH_ERROR_ENABLED) {
			diag_logf(DiagLevel::Info, "jitter", L"Vulkan constants: %hs could not be enabled", c.name);
			continue;
		}
		++hooked;
	}
	if (binding_offset_fn_ == nullptr)
		binding_offset_fn_ = reinterpret_cast<void *>(gdpa(dev, "vkGetDescriptorSetLayoutBindingOffsetEXT"));
	const bool first = hooked_.load(std::memory_order_acquire) == 0u;
	hooked_.store(hooked, std::memory_order_release);
	hookable_.store(hookable, std::memory_order_release);
	if (hooked != 0u) {
		const std::lock_guard<std::mutex> lock(devices_lock_);
		const uint64_t d = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device));
		if (std::find(devices_.begin(), devices_.end(), d) == devices_.end())
			devices_.push_back(d);
	}
	if (first && hooked != 0u)
		diag_logf(DiagLevel::Info, "jitter", L"Vulkan constants: hooked %u of the %u calls this device has that bind a "
			L"vertex shader's constants or map its memory: they prove a quad over the scene is placed by a camera",
			hooked, hookable);
	return hooked != 0u;
}

void VulkanConstants::each_hook(void (*fn)(void *user, const char *name, const void *target), void *user) noexcept
{
	if (fn == nullptr)
		return;
	for (const CallInfo &c : kCalls)
		if (const void *const t = g_calls[c.call].target.load(std::memory_order_acquire))
			fn(user, c.name, t);
}

void VulkanConstants::disable() noexcept
{
	for (Hooked &h : g_calls) {
		if (void *const t = h.target.load(std::memory_order_acquire))
			MH_DisableHook(t);
	}
	hooked_.store(0u, std::memory_order_release);
	const std::lock_guard<std::mutex> lock(devices_lock_);
	devices_.clear();
}

VulkanConstants &vulkan_constants() noexcept
{
	static VulkanConstants *const c = new VulkanConstants;
	return *c;
}

}

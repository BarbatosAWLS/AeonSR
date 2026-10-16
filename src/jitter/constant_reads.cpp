#include "aeon_sr/jitter/constant_reads.hpp"

#include <reshade.hpp>

#include <d3d10_1.h>
#include <d3d11_1.h>
#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <shared_mutex>
#include <vector>

namespace aeon_sr {

using namespace reshade::api;

bool ConstantReads::read(const Key &key, uint64_t now, float out[4])
{
	const std::lock_guard<std::mutex> lock(lock_);
	const auto it = entries_.find(key);
	if (it == entries_.end() || it->second.agree < kAgree || now - it->second.read_at > kFresh)
		return false;
	std::memcpy(out, it->second.value, 16u);
	return true;
}

bool ConstantReads::want(const Key &key)
{
	const std::lock_guard<std::mutex> lock(lock_);
	auto it = entries_.find(key);
	if (it == entries_.end()) {
		if (entries_.size() >= kMaxKeys)
			return false;
		it = entries_.emplace(key, Entry{}).first;
	}
	if (it->second.pending)
		return false;
	it->second.pending = true;
	return true;
}

bool ConstantReads::ask(const Key &key)
{
	const std::lock_guard<std::mutex> lock(lock_);
	if (entries_.find(key) != entries_.end())
		return true;
	if (entries_.size() >= kMaxKeys)
		return false;
	entries_.emplace(key, Entry{});
	return true;
}

void ConstantReads::landed(const Key &key, const float value[4], uint64_t now)
{
	const std::lock_guard<std::mutex> lock(lock_);
	const auto it = entries_.find(key);
	if (it == entries_.end())
		return;
	Entry &e = it->second;
	uint8_t zeros = 0;
	bool number = true;
	for (uint32_t l = 0; l < 4u; ++l) {
		const float x = value[l];
		number = number && x == x;
		if (x <= 1e-6f && x >= -1e-6f)
			zeros |= static_cast<uint8_t>(1u << l);
	}
	const bool same = number && e.agree != 0 && zeros == e.zeros && now - e.read_at <= kFresh;
	e.agree = !number ? 0u : same ? e.agree + 1u : 1u;
	e.zeros = zeros;
	std::memcpy(e.value, value, 16u);
	e.read_at = now;
	e.pending = false;
}

void ConstantReads::abandoned(const Key &key)
{
	const std::lock_guard<std::mutex> lock(lock_);
	const auto it = entries_.find(key);
	if (it != entries_.end())
		it->second.pending = false;
}

void ConstantReads::forget_shader(uint64_t shader)
{
	const std::lock_guard<std::mutex> lock(lock_);
	for (auto it = entries_.begin(); it != entries_.end();)
		it = it->first.shader == shader ? entries_.erase(it) : std::next(it);
}

void ConstantReads::clear()
{
	const std::lock_guard<std::mutex> lock(lock_);
	entries_.clear();
}

size_t ConstantReads::size()
{
	const std::lock_guard<std::mutex> lock(lock_);
	return entries_.size();
}

ConstantReads &constant_reads() noexcept
{
	static ConstantReads r;
	return r;
}

namespace {

struct Copy {
	ConstantReads::Key key;
	device_api api = device_api::d3d11;
	uint64_t device = 0;
	IUnknown *staging = nullptr;
	uint64_t started = 0;
};
std::mutex g_copies_lock;
std::vector<Copy> g_copies;

void release(Copy &c) noexcept
{
	if (c.staging != nullptr)
		c.staging->Release();
	c.staging = nullptr;
}

void started(Copy c)
{
	const std::lock_guard<std::mutex> lock(g_copies_lock);
	g_copies.push_back(c);
}

std::vector<Copy> take(uint64_t device)
{
	std::vector<Copy> mine;
	const std::lock_guard<std::mutex> lock(g_copies_lock);
	for (size_t i = g_copies.size(); i-- > 0;) {
		if (device == 0 || g_copies[i].device == device) {
			mine.push_back(g_copies[i]);
			g_copies.erase(g_copies.begin() + static_cast<std::ptrdiff_t>(i));
		}
	}
	return mine;
}

bool in_range(uint64_t at, uint64_t bytes)
{
	return at + 16u <= bytes;
}

}

bool d3d11_vertex_constant(command_list *cmd, uint64_t shader, uint32_t slot, uint32_t reg, uint64_t now,
	float out[4])
{
	if (cmd == nullptr || cmd->get_device()->get_api() != device_api::d3d11 || shader == 0 || slot >= 14u)
		return false;
	const ConstantReads::Key key{ shader, slot, reg };
	const bool known = constant_reads().read(key, now, out);
	auto *const ctx = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(cmd->get_native()));
	if (ctx == nullptr || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !constant_reads().want(key))
		return known;
	ID3D11Buffer *buffer = nullptr;
	UINT first = 0, count = 0;
	ID3D11DeviceContext1 *ctx1 = nullptr;
	if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&ctx1))) &&
		ctx1 != nullptr) {
		ctx1->VSGetConstantBuffers1(slot, 1, &buffer, &first, &count);
		ctx1->Release();
	} else {
		ctx->VSGetConstantBuffers(slot, 1, &buffer);
	}
	bool ok = buffer != nullptr && (count == 0 || reg < count);
	D3D11_BUFFER_DESC bd{};
	if (ok)
		buffer->GetDesc(&bd);
	const uint64_t at = (static_cast<uint64_t>(first) + reg) * 16u;
	ok = ok && in_range(at, bd.ByteWidth);
	ID3D11Device *dev = nullptr;
	ID3D11Buffer *staging = nullptr;
	if (ok) {
		ctx->GetDevice(&dev);
		D3D11_BUFFER_DESC sd{};
		sd.ByteWidth = 16u;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ok = dev != nullptr && SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging));
	}
	if (ok) {
		const D3D11_BOX box{ static_cast<UINT>(at), 0u, 0u, static_cast<UINT>(at + 16u), 1u, 1u };
		ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, buffer, 0, &box);
		started(Copy{ key, device_api::d3d11, reinterpret_cast<uintptr_t>(dev), staging, now });
	} else {
		constant_reads().abandoned(key);
	}
	if (dev != nullptr)
		dev->Release();
	if (buffer != nullptr)
		buffer->Release();
	return known;
}

bool d3d10_vertex_constant(command_list *cmd, uint64_t shader, uint32_t slot, uint32_t reg, uint64_t now,
	uint64_t game, float out[4])
{
	if (cmd == nullptr || cmd->get_device()->get_api() != device_api::d3d10 || shader == 0 || game == 0)
		return false;
	const ConstantReads::Key key{ shader, slot, reg };
	const bool known = constant_reads().read(key, now, out);
	if (!constant_reads().want(key))
		return known;
	auto *const dev = reinterpret_cast<ID3D10Device *>(static_cast<uintptr_t>(cmd->get_native()));
	auto *const buffer = reinterpret_cast<ID3D10Buffer *>(static_cast<uintptr_t>(game));
	D3D10_BUFFER_DESC bd{};
	buffer->GetDesc(&bd);
	const uint64_t at = static_cast<uint64_t>(reg) * 16u;
	ID3D10Buffer *staging = nullptr;
	bool ok = dev != nullptr && in_range(at, bd.ByteWidth);
	if (ok) {
		D3D10_BUFFER_DESC sd{};
		sd.ByteWidth = 16u;
		sd.Usage = D3D10_USAGE_STAGING;
		sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
		ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging));
	}
	if (ok) {
		const D3D10_BOX box{ static_cast<UINT>(at), 0u, 0u, static_cast<UINT>(at + 16u), 1u, 1u };
		dev->CopySubresourceRegion(staging, 0, 0, 0, 0, buffer, 0, &box);
		started(Copy{ key, device_api::d3d10, reinterpret_cast<uintptr_t>(dev), staging, now });
	} else {
		constant_reads().abandoned(key);
	}
	return known;
}

void constant_reads_poll(command_list *cmd, uint64_t now, bool wait)
{
	if (cmd == nullptr)
		return;
	const device_api api = cmd->get_device()->get_api();
	uint64_t device = 0;
	ID3D11DeviceContext *ctx = nullptr;
	if (api == device_api::d3d11) {
		ctx = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(cmd->get_native()));
		if (ctx == nullptr || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
			return;
		ID3D11Device *dev = nullptr;
		ctx->GetDevice(&dev);
		device = reinterpret_cast<uintptr_t>(dev);
		if (dev != nullptr)
			dev->Release();
	} else if (api == device_api::d3d10) {
		device = cmd->get_native();
	} else {
		return;
	}
	if (device == 0)
		return;
	std::vector<Copy> mine = take(device);
	std::vector<Copy> later;
	for (Copy &c : mine) {
		float v[4] = {};
		HRESULT hr = E_FAIL;
		if (c.api == device_api::d3d11) {
			auto *const staging = static_cast<ID3D11Buffer *>(c.staging);
			D3D11_MAPPED_SUBRESOURCE m{};
			hr = ctx->Map(staging, 0, D3D11_MAP_READ, wait ? 0u : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
			if (SUCCEEDED(hr)) {
				std::memcpy(v, m.pData, 16u);
				ctx->Unmap(staging, 0);
			}
		} else {
			auto *const staging = static_cast<ID3D10Buffer *>(c.staging);
			void *m = nullptr;
			hr = staging->Map(D3D10_MAP_READ, wait ? 0u : D3D10_MAP_FLAG_DO_NOT_WAIT, &m);
			if (SUCCEEDED(hr)) {
				std::memcpy(v, m, 16u);
				staging->Unmap();
			}
		}
		if (SUCCEEDED(hr)) {
			constant_reads().landed(c.key, v, now);
			release(c);
		} else if (hr == DXGI_ERROR_WAS_STILL_DRAWING && now - c.started <= ConstantReads::kGiveUp) {
			later.push_back(c);
		} else {
			constant_reads().abandoned(c.key);
			release(c);
		}
	}
	if (!later.empty()) {
		const std::lock_guard<std::mutex> lock(g_copies_lock);
		g_copies.insert(g_copies.end(), later.begin(), later.end());
	}
}

bool d3d11_vertex_position_replaced(command_list *cmd)
{
	if (cmd == nullptr || cmd->get_device()->get_api() != device_api::d3d11)
		return false;
	auto *const ctx = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(cmd->get_native()));
	if (ctx == nullptr)
		return false;
	ID3D11GeometryShader *gs = nullptr;
	ctx->GSGetShader(&gs, nullptr, nullptr);
	ID3D11DomainShader *ds = nullptr;
	ctx->DSGetShader(&ds, nullptr, nullptr);
	const bool replaced = gs != nullptr || ds != nullptr;
	if (gs != nullptr)
		gs->Release();
	if (ds != nullptr)
		ds->Release();
	return replaced;
}

void constant_reads_forget(uint64_t native_device)
{
	std::vector<Copy> gone = take(native_device);
	for (Copy &c : gone)
		release(c);
	constant_reads().clear();
}

namespace {

bool sees_vertex(shader_stage s) noexcept
{
	return (static_cast<uint32_t>(s) & static_cast<uint32_t>(shader_stage::vertex)) != 0u;
}

std::shared_mutex g_layouts_lock;
std::unordered_map<uint64_t, std::shared_ptr<const D3D12RootLayout>> g_layouts;
std::atomic<bool> g_vertex_tables{ false };

bool sane(const descriptor_range &r) noexcept
{
	return static_cast<uint32_t>(r.type) <= static_cast<uint32_t>(descriptor_type::acceleration_structure) &&
		r.count != 0u;
}

template <class Range>
bool table_ranges(uint32_t count, const Range *ranges, std::vector<D3D12TableRange> *out)
{
	if (ranges == nullptr || count == 0u || count > 64u)
		return false;
	for (uint32_t k = 0; k < count; ++k) {
		const descriptor_range &r = ranges[k];
		if (!sane(r))
			return false;
		if (r.type != descriptor_type::constant_buffer || !sees_vertex(r.visibility))
			continue;
		out->push_back(D3D12TableRange{ r.dx_register_index, r.dx_register_space, r.count, r.binding });
	}
	return true;
}

}

D3D12RootLayout d3d12_root_layout(uint32_t count, const pipeline_layout_param *params)
{
	D3D12RootLayout out;
	if (params == nullptr)
		return out;
	out.resize(std::min<uint32_t>(count, D3D12RootArgs::kParams));
	uint64_t words = 0;
	for (uint32_t i = 0; i < out.size(); ++i) {
		const pipeline_layout_param &p = params[i];
		D3D12RootParam &r = out[i];
		if (p.type == pipeline_layout_param_type::push_constants) {
			const uint64_t first = words;
			words += p.push_constants.count;
			if (!sees_vertex(p.push_constants.visibility) || words > D3D12RootArgs::kWords)
				continue;
			r.base = static_cast<uint32_t>(first);
			r.kind = D3D12RootParam::Kind::Constants;
			r.slot = p.push_constants.dx_register_index;
			r.space = p.push_constants.dx_register_space;
			r.count = p.push_constants.count;
			continue;
		}
		if (p.type == pipeline_layout_param_type::descriptor_table ||
			p.type == pipeline_layout_param_type::descriptor_table_with_flags) {
			std::vector<D3D12TableRange> ranges;
			const bool read = p.type == pipeline_layout_param_type::descriptor_table
				? table_ranges(p.descriptor_table.count, p.descriptor_table.ranges, &ranges)
				: table_ranges(p.descriptor_table_with_flags.count, p.descriptor_table_with_flags.ranges, &ranges);
			if (read && !ranges.empty()) {
				r.kind = D3D12RootParam::Kind::Table;
				r.ranges = std::move(ranges);
			}
			continue;
		}
		const descriptor_range *range = nullptr;
		if (p.type == pipeline_layout_param_type::push_descriptors && p.push_descriptors.count == 1u)
			range = &p.push_descriptors;
		else if (p.type == pipeline_layout_param_type::push_descriptors_with_ranges_and_flags &&
			p.descriptor_table_with_flags.count == 1u && p.descriptor_table_with_flags.ranges != nullptr)
			range = p.descriptor_table_with_flags.ranges;
		if (range != nullptr && range->type == descriptor_type::constant_buffer && range->count == 1u &&
			sees_vertex(range->visibility)) {
			r.kind = D3D12RootParam::Kind::View;
			r.slot = range->dx_register_index;
			r.space = range->dx_register_space;
		}
	}
	return out;
}

void d3d12_note_layout(uint64_t layout, uint32_t count, const pipeline_layout_param *params)
{
	if (layout == 0)
		return;
	auto made = std::make_shared<const D3D12RootLayout>(d3d12_root_layout(count, params));
	for (const D3D12RootParam &p : *made)
		if (p.kind == D3D12RootParam::Kind::Table)
			g_vertex_tables.store(true, std::memory_order_relaxed);
	const std::unique_lock<std::shared_mutex> lock(g_layouts_lock);
	g_layouts[layout] = std::move(made);
}

bool d3d12_vertex_tables_seen() noexcept
{
	return g_vertex_tables.load(std::memory_order_relaxed);
}

void d3d12_forget_layouts()
{
	const std::unique_lock<std::shared_mutex> lock(g_layouts_lock);
	g_layouts.clear();
	g_vertex_tables.store(false, std::memory_order_relaxed);
}

void d3d12_forget_layout(uint64_t layout)
{
	const std::unique_lock<std::shared_mutex> lock(g_layouts_lock);
	g_layouts.erase(layout);
}

std::shared_ptr<const D3D12RootLayout> d3d12_layout(uint64_t layout)
{
	const std::shared_lock<std::shared_mutex> lock(g_layouts_lock);
	const auto it = g_layouts.find(layout);
	return it != g_layouts.end() ? it->second : nullptr;
}

void D3D12RootArgs::clear() noexcept
{
	layout = 0;
	params.reset();
	looked = false;
	words_set = 0;
	views_set = 0;
	tables_set = 0;
}

void D3D12RootArgs::use(uint64_t l)
{
	if (l == layout && looked)
		return;
	clear();
	layout = l;
	params = d3d12_layout(l);
	looked = true;
}

void D3D12RootArgs::constants(uint64_t l, uint32_t param, uint32_t first, uint32_t count, const uint32_t *values)
{
	use(l);
	if (params == nullptr || values == nullptr || param >= params->size())
		return;
	const D3D12RootParam &p = (*params)[param];
	if (p.kind != D3D12RootParam::Kind::Constants)
		return;
	for (uint32_t i = 0; i < count && first + i < p.count; ++i) {
		const uint32_t w = p.base + first + i;
		if (w >= kWords)
			break;
		words[w] = values[i];
		words_set |= 1ull << w;
	}
}

void D3D12RootArgs::view(uint64_t l, uint32_t param, uint64_t buffer, uint64_t offset)
{
	use(l);
	if (params == nullptr || param >= params->size() || param >= kParams)
		return;
	if ((*params)[param].kind != D3D12RootParam::Kind::View)
		return;
	view_buffer[param] = buffer;
	view_offset[param] = offset;
	if (buffer != 0)
		views_set |= 1ull << param;
	else
		views_set &= ~(1ull << param);
}

void D3D12RootArgs::tables(uint64_t l, uint32_t first, uint32_t count, const uint64_t *handles)
{
	use(l);
	if (params == nullptr || handles == nullptr)
		return;
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t param = first + i;
		if (param >= params->size() || param >= kParams)
			break;
		if ((*params)[param].kind != D3D12RootParam::Kind::Table)
			continue;
		table[param] = handles[i];
		if (handles[i] != 0)
			tables_set |= 1ull << param;
		else
			tables_set &= ~(1ull << param);
	}
}

D3D12Place d3d12_root_place(const D3D12RootArgs &args, uint64_t layout, uint32_t space, uint32_t slot, uint32_t reg)
{
	D3D12Place out;
	if (layout == 0 || args.layout != layout || args.params == nullptr)
		return out;
	for (uint32_t i = 0; i < args.params->size(); ++i) {
		const D3D12RootParam &p = (*args.params)[i];
		if (p.kind == D3D12RootParam::Kind::Table) {
			for (const D3D12TableRange &r : p.ranges) {
				if (r.space != space || slot < r.slot || static_cast<uint64_t>(slot - r.slot) >= r.count)
					continue;
				if (i >= D3D12RootArgs::kParams || (args.tables_set & (1ull << i)) == 0u)
					return out;
				out.kind = D3D12Place::Kind::Table;
				out.table = args.table[i];
				out.index = r.offset + (slot - r.slot);
				out.offset = static_cast<uint64_t>(reg) * 16u;
				return out;
			}
			continue;
		}
		if (p.kind == D3D12RootParam::Kind::Other || p.slot != slot || p.space != space)
			continue;
		if (p.kind == D3D12RootParam::Kind::Constants) {
			bool any = false;
			for (uint32_t l = 0; l < 4u; ++l) {
				const uint64_t at = static_cast<uint64_t>(reg) * 4u + l;
				const uint64_t w = p.base + at;
				if (at < p.count && w < D3D12RootArgs::kWords && (args.words_set & (1ull << w)) != 0u) {
					std::memcpy(&out.value[l], &args.words[w], 4u);
					any = true;
				} else {
					out.value[l] = std::numeric_limits<float>::quiet_NaN();
				}
			}
			if (any)
				out.kind = D3D12Place::Kind::Value;
			return out;
		}
		if (i >= D3D12RootArgs::kParams || (args.views_set & (1ull << i)) == 0u)
			return out;
		out.kind = D3D12Place::Kind::View;
		out.buffer = args.view_buffer[i];
		out.offset = args.view_offset[i] + static_cast<uint64_t>(reg) * 16u;
		return out;
	}
	return out;
}

void D3D12Buffers::note(uint64_t buffer, uint64_t va, uint64_t bytes, const void *cpu)
{
	if (buffer == 0 || va == 0 || bytes < 16u || cpu == nullptr)
		return;
	const std::unique_lock<std::shared_mutex> lock(lock_);
	const auto old = va_of_.find(buffer);
	if (old != va_of_.end()) {
		const auto at = by_va_.find(old->second);
		if (at != by_va_.end() && at->second.buffer == buffer)
			by_va_.erase(at);
	}
	by_va_[va] = Entry{ buffer, bytes, static_cast<const uint8_t *>(cpu) };
	va_of_[buffer] = va;
}

void D3D12Buffers::forget(uint64_t buffer)
{
	{
		const std::shared_lock<std::shared_mutex> lock(lock_);
		if (va_of_.find(buffer) == va_of_.end())
			return;
	}
	const std::unique_lock<std::shared_mutex> lock(lock_);
	const auto it = va_of_.find(buffer);
	if (it == va_of_.end())
		return;
	const auto at = by_va_.find(it->second);
	if (at != by_va_.end() && at->second.buffer == buffer)
		by_va_.erase(at);
	va_of_.erase(it);
}

bool D3D12Buffers::read(uint64_t buffer, uint64_t offset, float out[4])
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	const auto it = va_of_.find(buffer);
	if (it == va_of_.end())
		return false;
	const auto at = by_va_.find(it->second);
	if (at == by_va_.end() || at->second.buffer != buffer || offset > at->second.bytes ||
		at->second.bytes - offset < 16u)
		return false;
	std::memcpy(out, at->second.cpu + offset, 16u);
	return true;
}

bool D3D12Buffers::read_va(uint64_t va, float out[4])
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	auto it = by_va_.upper_bound(va);
	if (it == by_va_.begin())
		return false;
	--it;
	const uint64_t offset = va - it->first;
	if (offset > it->second.bytes || it->second.bytes - offset < 16u)
		return false;
	std::memcpy(out, it->second.cpu + offset, 16u);
	return true;
}

size_t D3D12Buffers::size()
{
	const std::shared_lock<std::shared_mutex> lock(lock_);
	return va_of_.size();
}

void D3D12Buffers::clear()
{
	const std::unique_lock<std::shared_mutex> lock(lock_);
	by_va_.clear();
	va_of_.clear();
}

D3D12Buffers &d3d12_buffers() noexcept
{
	static D3D12Buffers b;
	return b;
}

const void *d3d12_map_cpu_buffer(uint64_t buffer, uint64_t *va, uint64_t *bytes)
{
	auto *const r = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(buffer));
	if (r == nullptr)
		return nullptr;
	D3D12_HEAP_PROPERTIES hp{};
	D3D12_HEAP_FLAGS hf{};
	if (FAILED(r->GetHeapProperties(&hp, &hf)))
		return nullptr;
	const bool cpu = hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == D3D12_HEAP_TYPE_GPU_UPLOAD ||
		(hp.Type == D3D12_HEAP_TYPE_CUSTOM && (hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
			hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK));
	const D3D12_RESOURCE_DESC desc = r->GetDesc();
	if (!cpu || desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width < 16u)
		return nullptr;
	void *p = nullptr;
	if (FAILED(r->Map(0, nullptr, &p)) || p == nullptr)
		return nullptr;
	*va = r->GetGPUVirtualAddress();
	*bytes = desc.Width;
	return p;
}

struct D3D12CbvHeaps::Page {
	std::atomic<uint64_t> e[kPage];
	Page()
	{
		for (std::atomic<uint64_t> &x : e)
			x.store(0, std::memory_order_relaxed);
	}
};

struct D3D12CbvHeaps::Heap {
	static constexpr uint32_t kPages = kMaxIndex / kPage;
	std::atomic<uint64_t> handle{ 0 };
	std::atomic<Page *> *pages = nullptr;
};

namespace {

uint64_t cbv_word(uint64_t va, uint64_t bytes) noexcept
{
	if (va == 0 || (va & 0xFFu) != 0u || bytes < 256u || bytes > 65536u)
		return 0;
	return va | ((bytes / 256u - 1u) & 0xFFu);
}

thread_local uint64_t t_last_heap = 0;
thread_local uint32_t t_last_slot = 0;

}

D3D12CbvHeaps::D3D12CbvHeaps() : heaps_(new Heap[kHeaps]) {}

D3D12CbvHeaps::~D3D12CbvHeaps()
{
	clear();
	for (uint32_t i = 0; i < kHeaps; ++i)
		delete[] heaps_[i].pages;
	delete[] heaps_;
}

D3D12CbvHeaps::Heap *D3D12CbvHeaps::heap_of(uint64_t heap) const
{
	if (heap == 0)
		return nullptr;
	const uint32_t n = heap_count_.load(std::memory_order_acquire);
	if (t_last_heap == heap && t_last_slot < n && heaps_[t_last_slot].handle.load(std::memory_order_relaxed) == heap)
		return &heaps_[t_last_slot];
	for (uint32_t i = 0; i < n; ++i) {
		if (heaps_[i].handle.load(std::memory_order_relaxed) == heap) {
			t_last_heap = heap;
			t_last_slot = i;
			return &heaps_[i];
		}
	}
	return nullptr;
}

D3D12CbvHeaps::Heap *D3D12CbvHeaps::make_heap(uint64_t heap)
{
	if (heap == 0)
		return nullptr;
	const std::lock_guard<std::mutex> lock(make_lock_);
	if (Heap *const h = heap_of(heap))
		return h;
	const uint32_t n = heap_count_.load(std::memory_order_relaxed);
	if (n >= kHeaps)
		return nullptr;
	if (heaps_[n].pages == nullptr) {
		heaps_[n].pages = new (std::nothrow) std::atomic<Page *>[Heap::kPages];
		if (heaps_[n].pages == nullptr)
			return nullptr;
		for (uint32_t k = 0; k < Heap::kPages; ++k)
			heaps_[n].pages[k].store(nullptr, std::memory_order_relaxed);
	}
	heaps_[n].handle.store(heap, std::memory_order_relaxed);
	heap_count_.store(n + 1u, std::memory_order_release);
	return &heaps_[n];
}

std::atomic<uint64_t> *D3D12CbvHeaps::entry(Heap *h, uint32_t index, bool make)
{
	if (h == nullptr || index >= kMaxIndex)
		return nullptr;
	std::atomic<Page *> &slot = h->pages[index / kPage];
	Page *p = slot.load(std::memory_order_acquire);
	if (p == nullptr) {
		if (!make || page_count_.load(std::memory_order_relaxed) >= kMaxPages)
			return nullptr;
		Page *const made = new (std::nothrow) Page;
		if (made == nullptr)
			return nullptr;
		Page *expected = nullptr;
		if (slot.compare_exchange_strong(expected, made, std::memory_order_acq_rel)) {
			page_count_.fetch_add(1, std::memory_order_relaxed);
			p = made;
		} else {
			delete made;
			p = expected;
		}
	}
	return &p->e[index % kPage];
}

void D3D12CbvHeaps::written(uint64_t heap, uint32_t index, uint64_t va, uint64_t bytes)
{
	const uint64_t w = cbv_word(va, bytes);
	if (w == 0) {
		cleared(heap, index, 1u);
		return;
	}
	if (std::atomic<uint64_t> *const e = entry(make_heap(heap), index, true)) {
		e->store(w, std::memory_order_relaxed);
		any_.store(true, std::memory_order_relaxed);
	} else if (std::atomic<uint64_t> *const old = entry(heap_of(heap), index, false)) {
		old->store(0, std::memory_order_relaxed);
	}
}

void D3D12CbvHeaps::cleared(uint64_t heap, uint32_t index, uint32_t count)
{
	if (!any_.load(std::memory_order_relaxed))
		return;
	Heap *const h = heap_of(heap);
	if (h == nullptr)
		return;
	for (uint32_t i = 0; i < count && index + i < kMaxIndex; ++i)
		if (std::atomic<uint64_t> *const e = entry(h, index + i, false))
			e->store(0, std::memory_order_relaxed);
}

void D3D12CbvHeaps::copied(uint64_t dst_heap, uint32_t dst_index, uint64_t src_heap, uint32_t src_index, uint32_t count)
{
	if (!any_.load(std::memory_order_relaxed))
		return;
	Heap *const s = heap_of(src_heap);
	Heap *d = heap_of(dst_heap);
	if (s == nullptr && d == nullptr)
		return;
	for (uint32_t i = 0; i < count; ++i) {
		if (dst_index + i >= kMaxIndex)
			break;
		const std::atomic<uint64_t> *const from = src_index + i < kMaxIndex ? entry(s, src_index + i, false) : nullptr;
		const uint64_t w = from != nullptr ? from->load(std::memory_order_relaxed) : 0u;
		if (w != 0u && d == nullptr)
			d = make_heap(dst_heap);
		if (std::atomic<uint64_t> *const to = entry(d, dst_index + i, w != 0u))
			to->store(w, std::memory_order_relaxed);
	}
}

bool D3D12CbvHeaps::find(uint64_t heap, uint32_t index, uint64_t *va, uint64_t *bytes) const
{
	const std::atomic<uint64_t> *const e = const_cast<D3D12CbvHeaps *>(this)->entry(heap_of(heap), index, false);
	const uint64_t w = e != nullptr ? e->load(std::memory_order_relaxed) : 0u;
	if (w == 0u)
		return false;
	*va = w & ~0xFFull;
	*bytes = ((w & 0xFFu) + 1u) * 256u;
	return true;
}

bool D3D12CbvHeaps::any() const noexcept
{
	return any_.load(std::memory_order_relaxed);
}

size_t D3D12CbvHeaps::pages() const noexcept
{
	return page_count_.load(std::memory_order_relaxed);
}

void D3D12CbvHeaps::clear()
{
	const std::lock_guard<std::mutex> lock(make_lock_);
	const uint32_t n = heap_count_.load(std::memory_order_relaxed);
	for (uint32_t i = 0; i < n; ++i) {
		for (uint32_t k = 0; heaps_[i].pages != nullptr && k < Heap::kPages; ++k)
			delete heaps_[i].pages[k].exchange(nullptr, std::memory_order_relaxed);
		heaps_[i].handle.store(0, std::memory_order_relaxed);
	}
	heap_count_.store(0, std::memory_order_release);
	page_count_.store(0, std::memory_order_relaxed);
	any_.store(false, std::memory_order_relaxed);
}

D3D12CbvHeaps &d3d12_cbv_heaps() noexcept
{
	static D3D12CbvHeaps *const h = new D3D12CbvHeaps;
	return *h;
}

uint8_t constant_zero_lanes(const float v[4], bool *number) noexcept
{
	uint8_t zeros = 0;
	bool n = true;
	for (uint32_t l = 0; l < 4u; ++l) {
		const float x = v[l];
		n = n && x == x;
		if (x <= 1e-6f && x >= -1e-6f)
			zeros |= static_cast<uint8_t>(1u << l);
	}
	if (number != nullptr)
		*number = n;
	return zeros;
}

void D3D12ConstantChecks::checked(const Key &key, const float *at_draw, const float *at_submit, uint64_t now)
{
	if (at_submit == nullptr)
		return;
	bool number = false;
	const uint8_t zeros = constant_zero_lanes(at_submit, &number);
	const std::lock_guard<std::mutex> lock(lock_);
	auto it = entries_.find(key);
	if (it == entries_.end()) {
		if (entries_.size() >= kMaxKeys)
			return;
		it = entries_.emplace(key, Entry{}).first;
		++per_pipeline_[key.pipeline];
	}
	Entry &e = it->second;
	const bool same = number && e.agree != 0 && zeros == e.zeros && now - e.read_at <= kFresh;
	e.agree = !number ? 0u : same ? e.agree + 1u : 1u;
	e.zeros = zeros;
	std::memcpy(e.value, at_submit, 16u);
	e.read_at = now;
	if (at_draw != nullptr) {
		bool drawn_number = false;
		const uint8_t drawn = constant_zero_lanes(at_draw, &drawn_number);
		if (number && drawn_number && drawn == zeros) {
			e.confirmed = std::min(e.confirmed + 1u, kConfirm);
		} else {
			e.confirmed = 0;
			++disagreed_;
		}
	}
}

bool D3D12ConstantChecks::trusted(const Key &key)
{
	const std::lock_guard<std::mutex> lock(lock_);
	const auto it = entries_.find(key);
	return it != entries_.end() && it->second.confirmed >= kConfirm;
}

bool D3D12ConstantChecks::agreed(const Key &key, uint64_t now, float out[4])
{
	const std::lock_guard<std::mutex> lock(lock_);
	const auto it = entries_.find(key);
	if (it == entries_.end() || it->second.agree < kAgree || now - it->second.read_at > kFresh)
		return false;
	std::memcpy(out, it->second.value, 16u);
	return true;
}

void D3D12ConstantChecks::forget_pipeline(uint64_t pipeline)
{
	const std::lock_guard<std::mutex> lock(lock_);
	const auto has = per_pipeline_.find(pipeline);
	if (has == per_pipeline_.end())
		return;
	per_pipeline_.erase(has);
	for (auto it = entries_.begin(); it != entries_.end();)
		it = it->first.pipeline == pipeline ? entries_.erase(it) : std::next(it);
}

void D3D12ConstantChecks::clear()
{
	const std::lock_guard<std::mutex> lock(lock_);
	entries_.clear();
	per_pipeline_.clear();
	disagreed_ = 0;
}

size_t D3D12ConstantChecks::size()
{
	const std::lock_guard<std::mutex> lock(lock_);
	return entries_.size();
}

uint64_t D3D12ConstantChecks::disagreed()
{
	const std::lock_guard<std::mutex> lock(lock_);
	return disagreed_;
}

D3D12ConstantChecks &d3d12_constant_checks() noexcept
{
	static D3D12ConstantChecks c;
	return c;
}

}

#include "aeon_sr/jitter/constant_reads.hpp"

#include <reshade.hpp>

#include <d3d10_1.h>
#include <d3d11_1.h>

#include <cstring>
#include <iterator>
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

}

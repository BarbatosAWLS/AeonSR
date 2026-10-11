#include "aeon_sr/core/present_drive.hpp"

#include <d3d9.h>
#include <d3d10_1.h>
#include <d3d11_1.h>

#include <mutex>

namespace aeon_sr {

namespace {

std::mutex g_state11_lock;
ID3D11Device *g_state11_device = nullptr;
ID3DDeviceContextState *g_state11 = nullptr;

ID3DDeviceContextState *state11_for(ID3D11Device *device)
{
	const std::lock_guard<std::mutex> lock(g_state11_lock);
	if (g_state11 != nullptr && g_state11_device == device)
		return g_state11;
	if (g_state11 != nullptr) {
		g_state11->Release();
		g_state11 = nullptr;
		g_state11_device = nullptr;
	}
	ID3D11Device1 *device1 = nullptr;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) || device1 == nullptr)
		return nullptr;
	const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
	D3D_FEATURE_LEVEL chosen{};
	ID3DDeviceContextState *state = nullptr;
	const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0
		? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0u;
	const HRESULT hr = device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
		__uuidof(ID3D11Device), &chosen, &state);
	device1->Release();
	if (FAILED(hr) || state == nullptr)
		return nullptr;
	g_state11 = state;
	g_state11_device = device;
	return g_state11;
}

std::string folder_key(std::string p)
{
	for (char &c : p) {
		if (c == '/')
			c = '\\';
		else if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	std::string out = p.rfind("\\\\", 0) == 0 ? "\\\\" : "";
	std::vector<std::string> pieces;
	for (size_t from = 0; from <= p.size();) {
		size_t end = p.find('\\', from);
		if (end == std::string::npos)
			end = p.size();
		const std::string piece = p.substr(from, end - from);
		if (piece == "..") {
			if (!pieces.empty() && !(pieces.size() == 1 && (pieces[0].back() == ':' || !out.empty())))
				pieces.pop_back();
		} else if (!piece.empty() && piece != ".") {
			pieces.push_back(piece);
		}
		from = end + 1;
	}
	for (size_t i = 0; i < pieces.size(); ++i) {
		if (i != 0)
			out += '\\';
		out += pieces[i];
	}
	return out;
}

bool absolute_path(const std::string &p)
{
	return (p.size() > 1 && p[1] == ':') || p.rfind("\\\\", 0) == 0 || p.rfind("//", 0) == 0;
}

using CreateStateBlock10 = HRESULT(WINAPI *)(ID3D10Device *, D3D10_STATE_BLOCK_MASK *, ID3D10StateBlock **);
using MaskEnableAll10 = HRESULT(WINAPI *)(D3D10_STATE_BLOCK_MASK *);

}

bool effect_paths_reach(const std::vector<std::string> &paths, const std::string &addon_dir,
	const std::string &base_dir)
{
	const std::string addon = folder_key(addon_dir);
	for (std::string p : paths) {
		while (!p.empty() && (p.front() == '"' || p.front() == ' '))
			p.erase(0, 1);
		while (!p.empty() && (p.back() == '"' || p.back() == ' '))
			p.pop_back();
		bool recursive = false;
		if (p.size() >= 2 && p.compare(p.size() - 2, 2, "**") == 0) {
			recursive = true;
			p.resize(p.size() - 2);
		} else if (!p.empty() && p.back() == '*') {
			p.pop_back();
		}
		const std::string key = folder_key(absolute_path(p) ? p : base_dir + "\\" + p);
		if (key == addon || (recursive && addon.size() > key.size() && addon.compare(0, key.size(), key) == 0 &&
				addon[key.size()] == '\\'))
			return true;
	}
	return false;
}

bool d3d9_device_lost(reshade::api::device *device) noexcept
{
	if (device == nullptr || device->get_api() != reshade::api::device_api::d3d9)
		return false;
	auto *const d9 = reinterpret_cast<IDirect3DDevice9 *>(device->get_native());
	if (d9 == nullptr)
		return false;
	const HRESULT hr = d9->TestCooperativeLevel();
	return hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET;
}

GameStateGuard::GameStateGuard(reshade::api::device *device, reshade::api::command_list *cmd_list) noexcept
{
	if (device == nullptr)
		return;
	api_ = device->get_api();
	switch (api_) {
	case reshade::api::device_api::d3d9: {
		auto *const d9 = reinterpret_cast<IDirect3DDevice9 *>(device->get_native());
		if (d9 == nullptr)
			return;
		for (DWORD i = 0; i < 4; ++i) {
			IDirect3DSurface9 *rt = nullptr;
			if (SUCCEEDED(d9->GetRenderTarget(i, &rt)))
				targets_[i] = rt;
		}
		IDirect3DSurface9 *ds = nullptr;
		if (SUCCEEDED(d9->GetDepthStencilSurface(&ds)))
			targets_[4] = ds;
		IDirect3DStateBlock9 *block = nullptr;
		if (SUCCEEDED(d9->CreateStateBlock(D3DSBT_ALL, &block)))
			saved_ = block;
		d9->AddRef();
		context_ = d9;
		return;
	}
	case reshade::api::device_api::d3d10: {
		auto *const d10 = reinterpret_cast<ID3D10Device *>(device->get_native());
		const HMODULE m = GetModuleHandleW(L"d3d10.dll");
		if (d10 == nullptr || m == nullptr)
			return;
		const auto create = reinterpret_cast<CreateStateBlock10>(GetProcAddress(m, "D3D10CreateStateBlock"));
		const auto enable_all = reinterpret_cast<MaskEnableAll10>(GetProcAddress(m, "D3D10StateBlockMaskEnableAll"));
		if (create == nullptr || enable_all == nullptr)
			return;
		D3D10_STATE_BLOCK_MASK mask{};
		ID3D10StateBlock *block = nullptr;
		if (SUCCEEDED(enable_all(&mask)) && SUCCEEDED(create(d10, &mask, &block)) && block != nullptr) {
			if (SUCCEEDED(block->Capture()))
				saved_ = block;
			else
				block->Release();
		}
		return;
	}
	case reshade::api::device_api::d3d11: {
		auto *const ctx = cmd_list != nullptr ? reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native()) : nullptr;
		if (ctx == nullptr || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
			return;
		ID3D11DeviceContext1 *ctx1 = nullptr;
		if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) || ctx1 == nullptr)
			return;
		ID3D11Device *dev = nullptr;
		ctx->GetDevice(&dev);
		ID3DDeviceContextState *const ours = dev != nullptr ? state11_for(dev) : nullptr;
		if (dev != nullptr)
			dev->Release();
		if (ours == nullptr) {
			ctx1->Release();
			return;
		}
		ID3DDeviceContextState *game = nullptr;
		ctx1->SwapDeviceContextState(ours, &game);
		saved_ = game;
		context_ = ctx1;
		return;
	}
	default:
		return;
	}
}

GameStateGuard::~GameStateGuard()
{
	switch (api_) {
	case reshade::api::device_api::d3d9: {
		auto *const d9 = static_cast<IDirect3DDevice9 *>(context_);
		if (d9 != nullptr) {
			if (targets_[0] != nullptr)
				d9->SetRenderTarget(0, static_cast<IDirect3DSurface9 *>(targets_[0]));
			for (DWORD i = 1; i < 4; ++i)
				d9->SetRenderTarget(i, static_cast<IDirect3DSurface9 *>(targets_[i]));
			d9->SetDepthStencilSurface(static_cast<IDirect3DSurface9 *>(targets_[4]));
		}
		if (saved_ != nullptr) {
			static_cast<IDirect3DStateBlock9 *>(saved_)->Apply();
			static_cast<IDirect3DStateBlock9 *>(saved_)->Release();
		}
		if (d9 != nullptr)
			d9->Release();
		for (void *&t : targets_)
			if (t != nullptr) {
				static_cast<IDirect3DSurface9 *>(t)->Release();
				t = nullptr;
			}
		return;
	}
	case reshade::api::device_api::d3d10:
		if (saved_ != nullptr) {
			static_cast<ID3D10StateBlock *>(saved_)->Apply();
			static_cast<ID3D10StateBlock *>(saved_)->Release();
		}
		return;
	case reshade::api::device_api::d3d11:
		if (context_ != nullptr) {
			auto *const ctx1 = static_cast<ID3D11DeviceContext1 *>(context_);
			if (saved_ != nullptr) {
				ctx1->ClearState();
				ID3DDeviceContextState *ours = nullptr;
				ctx1->SwapDeviceContextState(static_cast<ID3DDeviceContextState *>(saved_), &ours);
				if (ours != nullptr)
					ours->Release();
				static_cast<ID3DDeviceContextState *>(saved_)->Release();
			}
			ctx1->Release();
		}
		return;
	default:
		return;
	}
}

void GameStateGuard::release_cached() noexcept
{
	const std::lock_guard<std::mutex> lock(g_state11_lock);
	if (g_state11 != nullptr)
		g_state11->Release();
	g_state11 = nullptr;
	g_state11_device = nullptr;
}

}

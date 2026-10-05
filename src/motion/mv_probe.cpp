#include "aeon_sr/motion/mv_probe.hpp"

#include "aeon_sr/interop/blit_d3d12.hpp"

namespace aeon_sr {

void MvProbe11::release()
{
	if (staging != nullptr) { staging->Release(); staging = nullptr; }
	device = nullptr;
	width = height = 0;
	format = DXGI_FORMAT_UNKNOWN;
	pending = false;
	due_frame = next_copy_frame = 0;
}

void MvProbe11::restart() noexcept
{
	pending = false;
	next_copy_frame = 0;
}

bool MvProbe11::tick(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *mv,
	float screen_w, float screen_h, uint64_t frame, MvProbeResult &out)
{
	if (dev == nullptr || ctx == nullptr || mv == nullptr)
		return false;

	ID3D11Texture2D *tex = nullptr;
	if (FAILED(mv->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) || tex == nullptr)
		return false;
	D3D11_TEXTURE2D_DESC src{};
	tex->GetDesc(&src);
	tex->Release();

	if (!mv_probe_format_supported(src.Format)) {
		out.unsupported = true;
		out.valid = false;
		return false;
	}

	if (device != dev || staging == nullptr ||
		width != src.Width || height != src.Height || format != src.Format) {
		release();

		D3D11_TEXTURE2D_DESC d = src;
		d.Usage = D3D11_USAGE_STAGING;
		d.BindFlags = 0;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		d.MiscFlags = 0;
		if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging)))
			return false;
		device = dev;
		width = src.Width;
		height = src.Height;
		format = src.Format;
		next_copy_frame = frame;
	}

	bool completed = false;
	if (pending && frame >= due_frame) {
		D3D11_MAPPED_SUBRESOURCE m{};

		const HRESULT hr = ctx->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
		if (SUCCEEDED(hr) && m.pData != nullptr) {
			completed = mv_stats_from_image(m.pData, m.RowPitch, width, height, format,
				screen_w, screen_h, out);
			ctx->Unmap(staging, 0);
			out.frame = frame;
			pending = false;
			next_copy_frame = frame + kMvProbeInterval;
		} else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
			pending = false;
			next_copy_frame = frame + kMvProbeInterval;
		}
	}

	if (!pending && frame >= next_copy_frame) {
		ctx->CopyResource(staging, mv);
		pending = true;
		due_frame = frame + kMvProbeReadDelay;
	}
	return completed;
}

void MvProbe12::release()
{
	if (readback != nullptr) {
		if (mapped != nullptr)
			readback->Unmap(0, nullptr);
		readback->Release();
		readback = nullptr;
	}
	mapped = nullptr;
	device = nullptr;
	width = height = 0;
	row_pitch = 0;
	format = DXGI_FORMAT_UNKNOWN;
	pending = false;
	due_frame = next_copy_frame = 0;
}

void MvProbe12::restart() noexcept
{
	pending = false;
	next_copy_frame = 0;
}

bool MvProbe12::tick(ID3D12Device *dev, ID3D12GraphicsCommandList *cmd, ID3D12Resource *mv,
	D3D12_RESOURCE_STATES mv_state, float screen_w, float screen_h,
	uint64_t frame, MvProbeResult &out)
{
	if (dev == nullptr || cmd == nullptr || mv == nullptr)
		return false;

	const D3D12_RESOURCE_DESC src = mv->GetDesc();
	if (!mv_probe_format_supported(src.Format)) {
		out.unsupported = true;
		out.valid = false;
		return false;
	}

	const uint32_t src_w = static_cast<uint32_t>(src.Width);
	if (device != dev || readback == nullptr ||
		width != src_w || height != src.Height || format != src.Format) {
		release();

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
		UINT64 total = 0;
		dev->GetCopyableFootprints(&src, 0, 1, 0, &fp, nullptr, nullptr, &total);
		if (total == 0)
			return false;

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bd{};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.Format = DXGI_FORMAT_UNKNOWN;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
			return false;

		if (FAILED(readback->Map(0, nullptr, &mapped)) || mapped == nullptr) {
			release();
			return false;
		}
		device = dev;
		width = src_w;
		height = src.Height;
		format = src.Format;
		row_pitch = fp.Footprint.RowPitch;
		next_copy_frame = frame;
	}

	bool completed = false;
	if (pending && frame >= due_frame) {
		completed = mv_stats_from_image(mapped, row_pitch, width, height, format,
			screen_w, screen_h, out);
		out.frame = frame;
		pending = false;
		next_copy_frame = frame + kMvProbeInterval;
	}

	if (!pending && frame >= next_copy_frame) {
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = readback;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint.Offset = 0;
		dst.PlacedFootprint.Footprint.Format = format;
		dst.PlacedFootprint.Footprint.Width = width;
		dst.PlacedFootprint.Footprint.Height = height;
		dst.PlacedFootprint.Footprint.Depth = 1;
		dst.PlacedFootprint.Footprint.RowPitch = row_pitch;

		D3D12_TEXTURE_COPY_LOCATION srcloc{};
		srcloc.pResource = mv;
		srcloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcloc.SubresourceIndex = 0;

		barrier12(cmd, mv, mv_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		cmd->CopyTextureRegion(&dst, 0, 0, 0, &srcloc, nullptr);
		barrier12(cmd, mv, D3D12_RESOURCE_STATE_COPY_SOURCE, mv_state);
		pending = true;
		due_frame = frame + kMvProbeReadDelay;
	}
	return completed;
}

}

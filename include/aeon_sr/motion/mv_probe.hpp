#pragma once

#include "aeon_sr/motion/mv_stats.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>

namespace aeon_sr {

struct MvProbe11 {
	ID3D11Device *device = nullptr;
	ID3D11Texture2D *staging = nullptr;
	uint32_t width = 0, height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	bool pending = false;
	uint64_t due_frame = 0;
	uint64_t next_copy_frame = 0;

	void release();

	void restart() noexcept;

	bool tick(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *mv,
		float screen_w, float screen_h, uint64_t frame, MvProbeResult &out, bool written_before);

	bool write_due(uint64_t frame) const noexcept { return !pending && frame + 1u >= next_copy_frame; }

	void note_written(uint64_t frame) noexcept
	{
		if (written != frame) {
			written_prior = written;
			written = frame;
		}
	}
	bool written_before(uint64_t frame) const noexcept
	{
		const uint64_t before = written == frame ? written_prior : written;
		return before != 0 && before + 1u == frame;
	}
	uint64_t written = 0, written_prior = 0;
};

struct MvProbe12 {
	ID3D12Device *device = nullptr;
	ID3D12Resource *readback = nullptr;
	void *mapped = nullptr;
	uint32_t width = 0, height = 0;
	uint32_t row_pitch = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	bool pending = false;
	uint64_t due_frame = 0;
	uint64_t next_copy_frame = 0;

	void release();

	void restart() noexcept;
	bool tick(ID3D12Device *dev, ID3D12GraphicsCommandList *cmd, ID3D12Resource *mv,
		D3D12_RESOURCE_STATES mv_state, float screen_w, float screen_h,
		uint64_t frame, MvProbeResult &out);
};

}

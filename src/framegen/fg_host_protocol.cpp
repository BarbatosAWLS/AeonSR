#include "aeon_sr/framegen/fg_host_protocol.hpp"

#include <dxgiformat.h>

#include <cmath>

namespace aeon_sr::fg_host {

bool format_supported(uint32_t format) noexcept
{
	switch (static_cast<DXGI_FORMAT>(format)) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return true;
	default:
		return false;
	}
}

bool read_setup(const Block &block, Setup *out, const wchar_t **why) noexcept
{
	const uint32_t magic = block.magic, version = block.version, size = block.block_size;
	if (magic != kMagic || version != kVersion || size != kBlockSize) {
		*why = L"the add-on and AeonSRHost.exe are from different builds";
		return false;
	}
	Setup s;
	s.generator = block.generator;
	s.width = block.width;
	s.height = block.height;
	s.format = block.format;
	s.adapter_luid_low = block.adapter_luid_low;
	s.adapter_luid_high = block.adapter_luid_high;
	const uint32_t outputs = block.output_count;
	if (s.generator < 1 || s.generator > 3) {
		*why = L"a frame generator that is not a vendor's";
		return false;
	}
	if (s.width == 0 || s.height == 0 || s.width > 16384 || s.height > 16384) {
		*why = L"a frame size no frame generator takes";
		return false;
	}
	if (!format_supported(s.format)) {
		*why = L"a frame format no frame generator takes";
		return false;
	}
	if (outputs != kMaxOutputs) {
		*why = L"the shared outputs are not the five this host takes";
		return false;
	}
	*out = s;
	return true;
}

bool read_request(const volatile Block &block, Request *out) noexcept
{
	Request r;
	r.seq = block.seq;
	const uint32_t command = block.command;
	r.generated = block.generated;
	r.wait_value = block.wait_value;
	r.signal_value = block.signal_value;
	r.reset = block.reset != 0;
	r.color_space = block.color_space;
	r.frame_ms = block.frame_ms;
	r.window = block.window;
	switch (command) {
	case static_cast<uint32_t>(Command::Frame):
		if (r.generated < 1 || r.generated > kMaxOutputs || r.wait_value == 0 || r.signal_value == 0 ||
				r.color_space > 2 || !std::isfinite(r.frame_ms) || r.frame_ms < 0.0f || r.frame_ms > 10000.0f)
			return false;
		break;
	case static_cast<uint32_t>(Command::Reset):
	case static_cast<uint32_t>(Command::Quit):
		break;
	default:
		return false;
	}
	r.command = static_cast<Command>(command);
	*out = r;
	return true;
}

}

#include "aeon_sr/framegen/window_host_protocol.hpp"

#include <dxgiformat.h>

namespace aeon_sr::window_host {

bool format_presentable(uint32_t format) noexcept
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
	s.width = block.width;
	s.height = block.height;
	s.format = block.format;
	const uint32_t cs = block.color_space;
	s.adapter_luid_low = block.adapter_luid_low;
	s.adapter_luid_high = block.adapter_luid_high;
	const uint32_t shared = block.shared_count;
	if (s.width == 0 || s.height == 0 || s.width > 16384 || s.height > 16384) {
		*why = L"a frame size no swap chain takes";
		return false;
	}
	if (!format_presentable(s.format)) {
		*why = L"a frame format the window does not present";
		return false;
	}
	if (cs > static_cast<uint32_t>(ColorSpace::Pq)) {
		*why = L"an unknown colour space";
		return false;
	}
	if (shared != kShared) {
		*why = L"the shared textures are not the three this host takes";
		return false;
	}
	s.color_space = static_cast<ColorSpace>(cs);
	*out = s;
	return true;
}

bool read_request(const volatile Block &block, Request *out) noexcept
{
	Request r;
	r.seq = block.seq;
	const uint32_t command = block.command;
	r.shared_index = block.shared_index;
	r.ready_value = block.ready_value;
	r.done_value = block.done_value;
	r.sync_interval = block.sync_interval;
	r.visible = block.visible != 0;
	r.x = block.x;
	r.y = block.y;
	r.cw = block.cw;
	r.ch = block.ch;
	switch (command) {
	case static_cast<uint32_t>(Command::Present):
		if (r.shared_index >= kShared || r.sync_interval > 4 || r.ready_value == 0 || r.done_value == 0)
			return false;
		break;
	case static_cast<uint32_t>(Command::Window):
		if (r.visible && (r.cw <= 0 || r.ch <= 0 || r.cw > 32768 || r.ch > 32768 || r.x < -65536 || r.x > 65536 ||
				r.y < -65536 || r.y > 65536))
			return false;
		break;
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

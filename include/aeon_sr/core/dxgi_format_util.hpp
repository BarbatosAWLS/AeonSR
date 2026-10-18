#pragma once

#include <dxgiformat.h>

namespace aeon_sr {

DXGI_FORMAT format_family(DXGI_FORMAT fmt) noexcept;

bool formats_copy_compatible(DXGI_FORMAT a, DXGI_FORMAT b) noexcept;

bool format_supports_uav(DXGI_FORMAT fmt) noexcept;

DXGI_FORMAT color_read_format(DXGI_FORMAT fmt) noexcept;
DXGI_FORMAT depth_read_format(DXGI_FORMAT fmt) noexcept;
DXGI_FORMAT view_format_for(DXGI_FORMAT fmt) noexcept;

bool is_srgb(DXGI_FORMAT f) noexcept;

DXGI_FORMAT resolve_scratch_format(DXGI_FORMAT backbuffer_fmt) noexcept;

DXGI_FORMAT upscaler_scratch_format(DXGI_FORMAT backbuffer_fmt) noexcept;

}

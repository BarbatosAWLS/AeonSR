#pragma once

#include <dxgiformat.h>

namespace aeon_sr {

DXGI_FORMAT format_family(DXGI_FORMAT fmt) noexcept;

bool formats_copy_compatible(DXGI_FORMAT a, DXGI_FORMAT b) noexcept;

bool format_supports_uav(DXGI_FORMAT fmt) noexcept;

DXGI_FORMAT view_format_for(DXGI_FORMAT fmt) noexcept;

DXGI_FORMAT resolve_scratch_format(DXGI_FORMAT backbuffer_fmt) noexcept;

}

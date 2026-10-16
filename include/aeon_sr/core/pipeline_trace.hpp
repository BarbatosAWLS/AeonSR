#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"

#include <cstdint>
#include <string>

namespace aeon_sr {

void register_pipeline_trace(unsigned int key, const std::wstring &dir);
void unregister_pipeline_trace();

const char *pipeline_trace_format_name(uint32_t dxgi_format) noexcept;
uint32_t pipeline_trace_format_bytes(uint32_t dxgi_format) noexcept;
bool pipeline_trace_two_channel(uint32_t dxgi_format) noexcept;

}

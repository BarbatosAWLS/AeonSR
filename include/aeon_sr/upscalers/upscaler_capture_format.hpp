#pragma once

#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <cstdint>
#include <string>

namespace aeon_sr {

struct CapturedPlane {
	std::string file;
	uint32_t width = 0, height = 0;
	uint32_t dxgi_format = 0;
	uint32_t row_bytes = 0;
};

struct CapturedFrameInfo {
	static constexpr uint32_t kVersion = 1;
	uint32_t version = kVersion;
	uint32_t index = 0;
	uint64_t sequence = 0;
	std::string upscaler;
	std::string fsr_provider;
	UpscalerParams params;
	CapturedPlane color, mvec, depth;
	CapturedPlane drawn;
	float split_shift_x = 0.0f, split_shift_y = 0.0f;
	float split_u = 0.0f, split_v = 0.0f;
};

std::string format_captured_frame(const CapturedFrameInfo &f);
bool parse_captured_frame(const std::string &text, CapturedFrameInfo *out);

std::wstring captured_frame_dir(const std::wstring &root, uint32_t index);

enum class ScopePlane : uint32_t { Drawn, UpscalerIn, Upscaled, Final, Mask, Count };
inline constexpr uint32_t kScopeStagePlanes = static_cast<uint32_t>(ScopePlane::Count);
const char *scope_plane_name(ScopePlane p) noexcept;

enum class ScopeColumn : uint32_t {
	N, Frame, Seq, PointX, PointY, ComputedDrawX, ComputedDrawY, DrawnX, DrawnY, DrawnValid, ArmedSerial,
	DrawnSerial, MovedDraws, PlainDraws, ElsewhereDraws, Empty, Plan, ToldX, ToldY, InFrame, ShiftX, ShiftY,
	SplitApplied, HudDrawX, HudDrawY, HudMovedDraws, HudGridDraws, Restore, RestoreMembers, RestoreHeld, Fault,
	FaultActive,
	Index, DrawnIndex, UnknownDraws, ArmedPending, Ran, Register, RenderW, RenderH, TimeMs,
	Count,
};
inline constexpr uint32_t kScopeColumns = static_cast<uint32_t>(ScopeColumn::Count);
const char *scope_column_name(ScopeColumn c) noexcept;

struct ScopeTraceRow {
	double v[kScopeColumns];
	std::string plan, fault;
	ScopeTraceRow() noexcept;
	void set(ScopeColumn c, double x) noexcept { v[static_cast<uint32_t>(c)] = x; }
	double get(ScopeColumn c) const noexcept { return v[static_cast<uint32_t>(c)]; }
	bool has(ScopeColumn c) const noexcept;
};

std::string format_scope_trace_header();
std::string format_scope_trace_row(const ScopeTraceRow &r);
std::string format_scope_fmt(uint32_t width, uint32_t height, uint32_t dxgi_format, uint32_t row_bytes);
std::wstring scope_plane_path(const std::wstring &dir, uint32_t n, ScopePlane p);
std::string format_scope_txt(const std::string &header, uint32_t frames);
void count_scope_mask(const uint8_t *bytes, size_t count, uint32_t *members, uint32_t *held) noexcept;

}

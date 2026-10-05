#include "aeon_sr/upscalers/upscaler_capture_format.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

namespace aeon_sr {
namespace {

void put(std::string *out, const char *key, const std::string &value)
{
	*out += key;
	*out += " = ";
	*out += value;
	*out += '\n';
}

std::string unum(uint64_t v)
{
	return std::to_string(v);
}

std::string fnum(float v)
{
	char buf[32]{};
	std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
	return buf;
}

void put_plane(std::string *out, const char *name, const CapturedPlane &p)
{
	const std::string k = name;
	put(out, (k + ".file").c_str(), p.file);
	put(out, (k + ".width").c_str(), unum(p.width));
	put(out, (k + ".height").c_str(), unum(p.height));
	put(out, (k + ".dxgi_format").c_str(), unum(p.dxgi_format));
	put(out, (k + ".row_bytes").c_str(), unum(p.row_bytes));
}

std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
		++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
		--b;
	return s.substr(a, b - a);
}

}

std::string format_captured_frame(const CapturedFrameInfo &f)
{
	std::string out;
	put(&out, "version", unum(f.version));
	put(&out, "index", unum(f.index));
	put(&out, "sequence", unum(f.sequence));
	put(&out, "upscaler", f.upscaler);
	put(&out, "fsr_provider", f.fsr_provider);
	const UpscalerParams &p = f.params;
	put(&out, "quality_mode", unum(p.quality_mode));
	put(&out, "render_preset", unum(p.render_preset));
	put(&out, "render_scale", fnum(p.render_scale));
	put(&out, "render_width", unum(p.render_width));
	put(&out, "render_height", unum(p.render_height));
	put(&out, "sharpness", fnum(p.sharpness));
	put(&out, "frame_time_ms", fnum(p.frame_time_ms));
	put(&out, "reset", unum(p.reset ? 1u : 0u));
	put(&out, "depth_inverted", unum(p.depth_inverted ? 1u : 0u));
	put(&out, "jitter_x", fnum(p.jitter_x));
	put(&out, "jitter_y", fnum(p.jitter_y));
	put(&out, "jitter_in_frame", unum(p.jitter_in_frame ? 1u : 0u));
	put(&out, "color_space", unum(p.color_space));
	put_plane(&out, "color", f.color);
	put_plane(&out, "mvec", f.mvec);
	put_plane(&out, "depth", f.depth);
	put_plane(&out, "drawn", f.drawn);
	put(&out, "split_shift_x", fnum(f.split_shift_x));
	put(&out, "split_shift_y", fnum(f.split_shift_y));
	put(&out, "split_u", fnum(f.split_u));
	put(&out, "split_v", fnum(f.split_v));
	return out;
}

bool parse_captured_frame(const std::string &text, CapturedFrameInfo *out)
{
	std::map<std::string, std::string> kv;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
	}
	bool ok = true;
	const auto str = [&](const char *k, std::string *v) {
		const auto it = kv.find(k);
		if (it == kv.end()) {
			ok = false;
			return;
		}
		*v = it->second;
	};
	const auto u64 = [&](const char *k, uint64_t *v) {
		std::string s;
		str(k, &s);
		char *end = nullptr;
		*v = std::strtoull(s.c_str(), &end, 10);
		if (s.empty() || end == nullptr || *end != '\0')
			ok = false;
	};
	const auto u32 = [&](const char *k, uint32_t *v) {
		uint64_t w = 0;
		u64(k, &w);
		*v = static_cast<uint32_t>(w);
	};
	const auto flag = [&](const char *k, bool *v) {
		uint32_t w = 0;
		u32(k, &w);
		*v = w != 0;
	};
	const auto f32 = [&](const char *k, float *v) {
		std::string s;
		str(k, &s);
		char *end = nullptr;
		*v = std::strtof(s.c_str(), &end);
		if (s.empty() || end == nullptr || *end != '\0')
			ok = false;
	};
	const auto plane = [&](const std::string &name, CapturedPlane *p) {
		str((name + ".file").c_str(), &p->file);
		u32((name + ".width").c_str(), &p->width);
		u32((name + ".height").c_str(), &p->height);
		u32((name + ".dxgi_format").c_str(), &p->dxgi_format);
		u32((name + ".row_bytes").c_str(), &p->row_bytes);
	};

	CapturedFrameInfo f;
	u32("version", &f.version);
	if (!ok || f.version != CapturedFrameInfo::kVersion)
		return false;
	u32("index", &f.index);
	u64("sequence", &f.sequence);
	str("upscaler", &f.upscaler);
	str("fsr_provider", &f.fsr_provider);
	UpscalerParams &p = f.params;
	u32("quality_mode", &p.quality_mode);
	u32("render_preset", &p.render_preset);
	f32("render_scale", &p.render_scale);
	u32("render_width", &p.render_width);
	u32("render_height", &p.render_height);
	f32("sharpness", &p.sharpness);
	f32("frame_time_ms", &p.frame_time_ms);
	flag("reset", &p.reset);
	flag("depth_inverted", &p.depth_inverted);
	f32("jitter_x", &p.jitter_x);
	f32("jitter_y", &p.jitter_y);
	flag("jitter_in_frame", &p.jitter_in_frame);
	u32("color_space", &p.color_space);
	plane("color", &f.color);
	plane("mvec", &f.mvec);
	plane("depth", &f.depth);
	if (!ok || f.color.file.empty() || f.mvec.file.empty())
		return false;
	if (kv.count("drawn.file") != 0) {
		plane("drawn", &f.drawn);
		f32("split_shift_x", &f.split_shift_x);
		f32("split_shift_y", &f.split_shift_y);
		f32("split_u", &f.split_u);
		f32("split_v", &f.split_v);
		if (!ok)
			return false;
	}
	*out = f;
	return true;
}

std::wstring captured_frame_dir(const std::wstring &root, uint32_t index)
{
	wchar_t name[32]{};
	std::swprintf(name, 32, L"frame_%02u", index);
	return root + L"\\" + name;
}

const char *scope_plane_name(ScopePlane p) noexcept
{
	switch (p) {
	case ScopePlane::Drawn: return "drawn";
	case ScopePlane::UpscalerIn: return "upscaler_in";
	case ScopePlane::Upscaled: return "upscaled";
	case ScopePlane::Final: return "final";
	case ScopePlane::Mask: return "mask";
	case ScopePlane::Count: break;
	}
	return "";
}

const char *scope_column_name(ScopeColumn c) noexcept
{
	static const char *const kNames[kScopeColumns] = {
		"n", "frame", "seq", "point_x", "point_y", "computed_draw_x", "computed_draw_y", "drawn_x", "drawn_y",
		"drawn_valid", "armed_serial", "drawn_serial", "moved_draws", "plain_draws", "elsewhere_draws", "empty",
		"plan", "told_x", "told_y", "in_frame", "shift_x", "shift_y", "split_applied", "hud_draw_x", "hud_draw_y",
		"hud_moved_draws", "hud_grid_draws", "restore", "restore_members", "restore_held", "fault", "fault_active",
		"index", "drawn_index", "unknown_draws", "armed_pending", "ran", "register", "render_w", "render_h",
		"time_ms",
	};
	const uint32_t i = static_cast<uint32_t>(c);
	return i < kScopeColumns ? kNames[i] : "";
}

ScopeTraceRow::ScopeTraceRow() noexcept
{
	for (double &x : v)
		x = std::nan("");
}

bool ScopeTraceRow::has(ScopeColumn c) const noexcept
{
	return !std::isnan(get(c));
}

std::string format_scope_trace_header()
{
	std::string out;
	for (uint32_t i = 0; i < kScopeColumns; ++i) {
		if (i != 0)
			out += ',';
		out += scope_column_name(static_cast<ScopeColumn>(i));
	}
	out += '\n';
	return out;
}

std::string format_scope_trace_row(const ScopeTraceRow &r)
{
	std::string out;
	for (uint32_t i = 0; i < kScopeColumns; ++i) {
		if (i != 0)
			out += ',';
		const ScopeColumn c = static_cast<ScopeColumn>(i);
		if (c == ScopeColumn::Plan) {
			out += r.plan;
			continue;
		}
		if (c == ScopeColumn::Fault) {
			out += r.fault;
			continue;
		}
		double x = r.v[i];
		if (std::isnan(x))
			continue;
		if (x == 0.0)
			x = 0.0;
		char buf[40]{};
		if (x == std::floor(x) && std::fabs(x) < 1e15)
			std::snprintf(buf, sizeof buf, "%.0f", x);
		else
			std::snprintf(buf, sizeof buf, "%.9g", x);
		out += buf;
	}
	out += '\n';
	return out;
}

std::string format_scope_fmt(uint32_t width, uint32_t height, uint32_t dxgi_format, uint32_t row_bytes)
{
	return unum(width) + " " + unum(height) + " " + unum(dxgi_format) + " " + unum(row_bytes) + "\n";
}

std::wstring scope_plane_path(const std::wstring &dir, uint32_t n, ScopePlane p)
{
	wchar_t name[64]{};
	std::swprintf(name, 64, L"f_%04u_%hs.bin", n, scope_plane_name(p));
	return dir + L"\\" + name;
}

std::string format_scope_txt(const std::string &header, uint32_t frames)
{
	std::string out = "scope_version=1\n";
	std::istringstream in(header);
	std::string line;
	while (std::getline(in, line)) {
		const std::string key = trim(line.substr(0, line.find('=')));
		if (line.empty() || key == "scope_version" || key == "frames" || key == "planes" || key == "plane_ext")
			continue;
		out += line;
		out += '\n';
	}
	out += "frames=" + unum(frames) + "\n";
	out += "planes=";
	for (uint32_t i = 0; i < kScopeStagePlanes; ++i)
		out += std::string(i != 0 ? "," : "") + scope_plane_name(static_cast<ScopePlane>(i));
	out += "\nplane_ext=bin\n";
	return out;
}

void count_scope_mask(const uint8_t *bytes, size_t count, uint32_t *members, uint32_t *held) noexcept
{
	uint32_t m = 0, h = 0;
	for (size_t i = 0; i < count; ++i) {
		m += bytes[i] != 0u ? 1u : 0u;
		h += bytes[i] == 128u ? 1u : 0u;
	}
	*members = m;
	*held = h;
}

}

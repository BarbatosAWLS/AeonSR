#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/core/pipeline_trace.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/frame_inputs.hpp"

#include <windows.h>
#include <dxgiformat.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aeon_sr {

using namespace reshade::api;

const char *pipeline_trace_format_name(uint32_t f) noexcept
{
	switch (f) {
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: return "R32G32B32A32_TYPELESS";
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
	case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
	case DXGI_FORMAT_R16G16B16A16_SNORM: return "R16G16B16A16_SNORM";
	case DXGI_FORMAT_R32G32_TYPELESS: return "R32G32_TYPELESS";
	case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
	case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
	case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
	case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
	case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
	case DXGI_FORMAT_R8G8B8A8_SNORM: return "R8G8B8A8_SNORM";
	case DXGI_FORMAT_R16G16_TYPELESS: return "R16G16_TYPELESS";
	case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
	case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
	case DXGI_FORMAT_R16G16_SNORM: return "R16G16_SNORM";
	case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
	case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
	case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
	case DXGI_FORMAT_R32_UINT: return "R32_UINT";
	case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
	case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
	case DXGI_FORMAT_R8G8_TYPELESS: return "R8G8_TYPELESS";
	case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
	case DXGI_FORMAT_R8G8_SNORM: return "R8G8_SNORM";
	case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
	case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
	case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
	case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
	case DXGI_FORMAT_R8_TYPELESS: return "R8_TYPELESS";
	case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
	case DXGI_FORMAT_A8_UNORM: return "A8_UNORM";
	case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
	case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
	case DXGI_FORMAT_B8G8R8X8_TYPELESS: return "B8G8R8X8_TYPELESS";
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return "B8G8R8X8_UNORM_SRGB";
	case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1";
	case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM_SRGB: return "BC2";
	case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3";
	case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_SNORM: return "BC4";
	case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_SNORM: return "BC5";
	case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7";
	default: return nullptr;
	}
}

uint32_t pipeline_trace_format_bytes(uint32_t f) noexcept
{
	switch (f) {
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return 16;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		return 8;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return 4;
	case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_UNORM:
		return 2;
	case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM:
		return 1;
	default:
		return 0;
	}
}

bool pipeline_trace_two_channel(uint32_t f) noexcept
{
	switch (f) {
	case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R8G8_SNORM:
		return true;
	default:
		return false;
	}
}

namespace {

constexpr uint32_t kTextFrames = 3;
constexpr uint32_t kMaxDumps = 96;

struct ShaderInfo {
	uint32_t hash = 0;
	uint32_t stage = 0;
	std::vector<uint8_t> code;
};

struct Group {
	uint64_t vs = 0, ps = 0;
	std::string srv;
	std::string cb;
	std::string vp;
	uint32_t draws = 0;
	uint64_t elements = 0;
	uint32_t max_elements = 0;
	uint32_t max_instances = 0;
};

struct State {
	std::mutex lock;
	unsigned int key = 0x76u;
	std::wstring dir;
	device *game = nullptr;

	std::unordered_map<uint64_t, resource_desc> resources;
	std::unordered_map<uint64_t, ShaderInfo> shaders;
	std::unordered_map<uint64_t, uint32_t> ids;
	uint32_t next_id = 1;

	uint32_t phase = 0;
	uint32_t frame = 0;
	std::wstring run_dir;
	FILE *text = nullptr;
	uint32_t dumps = 0;
	uint32_t pass = 0;
	bool pass_wrote = false;
	resource rts[8]{};
	uint32_t rt_count = 0;
	resource ds{};
	viewport vp{};
	uint64_t vs = 0, ps = 0, cs = 0;
	resource ps_srv[16]{};
	resource cs_srv[16]{};
	uint32_t vs_cb_size[16]{};
	Group group;
	std::unordered_map<uint64_t, bool> used_shaders;
	std::vector<std::string> table;
};

State &st()
{
	static State s;
	return s;
}

uint32_t fnv1a(const void *p, size_t n)
{
	uint32_t h = 2166136261u;
	const uint8_t *b = static_cast<const uint8_t *>(p);
	for (size_t i = 0; i < n; ++i) {
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}

std::string usage_letters(resource_usage u)
{
	std::string s;
	if ((u & resource_usage::render_target) != resource_usage::undefined) s += "RT ";
	if ((u & resource_usage::depth_stencil) != resource_usage::undefined) s += "DS ";
	if ((u & resource_usage::shader_resource) != resource_usage::undefined) s += "SR ";
	if ((u & resource_usage::unordered_access) != resource_usage::undefined) s += "UA ";
	if (!s.empty())
		s.pop_back();
	return s;
}

std::string describe_format(uint32_t f)
{
	const char *n = pipeline_trace_format_name(f);
	if (n != nullptr)
		return n;
	char b[24];
	std::snprintf(b, sizeof(b), "format%u", f);
	return b;
}

std::string name_of(State &s, resource r)
{
	if (r.handle == 0)
		return "-";
	auto it = s.ids.find(r.handle);
	uint32_t id;
	if (it == s.ids.end()) {
		id = s.next_id++;
		s.ids.emplace(r.handle, id);
	} else {
		id = it->second;
	}
	char b[16];
	std::snprintf(b, sizeof(b), "r%u", id);
	return b;
}

const resource_desc *desc_of(State &s, resource r)
{
	auto it = s.resources.find(r.handle);
	if (it != s.resources.end())
		return &it->second;
	if (s.game == nullptr || r.handle == 0)
		return nullptr;
	return &s.resources.emplace(r.handle, s.game->get_resource_desc(r)).first->second;
}

void note_table(State &s, resource r)
{
	if (r.handle == 0)
		return;
	const std::string n = name_of(s, r);
	for (const std::string &t : s.table) {
		if (t.compare(0, n.size() + 1, n + " ") == 0)
			return;
	}
	char b[256];
	const resource_desc *const known = desc_of(s, r);
	if (known == nullptr) {
		std::snprintf(b, sizeof(b), "%s unknown", n.c_str());
	} else {
		const resource_desc &d = *known;
		if (d.type == resource_type::buffer)
			std::snprintf(b, sizeof(b), "%s buffer %llu bytes %s", n.c_str(),
				static_cast<unsigned long long>(d.buffer.size), usage_letters(d.usage).c_str());
		else
			std::snprintf(b, sizeof(b), "%s %ux%u x%u mips %u samples %u %s %s", n.c_str(), d.texture.width,
				d.texture.height, d.texture.depth_or_layers, d.texture.levels, d.texture.samples,
				describe_format(static_cast<uint32_t>(d.texture.format)).c_str(), usage_letters(d.usage).c_str());
	}
	s.table.emplace_back(b);
}

std::string size_of(State &s, resource r)
{
	const resource_desc *const d = desc_of(s, r);
	if (d == nullptr || d->type == resource_type::buffer)
		return "";
	char b[64];
	std::snprintf(b, sizeof(b), "%ux%u %s", d->texture.width, d->texture.height,
		describe_format(static_cast<uint32_t>(d->texture.format)).c_str());
	return b;
}

std::string shader_name(State &s, uint64_t p)
{
	if (p == 0)
		return "-";
	auto it = s.shaders.find(p);
	if (it == s.shaders.end())
		return "?";
	s.used_shaders[p] = true;
	char b[16];
	std::snprintf(b, sizeof(b), "%c%08x", static_cast<char>(it->second.stage), it->second.hash);
	return b;
}

bool recording(const State &s) { return s.phase >= 2 && s.text != nullptr; }

bool recording(const State &s, command_list *cmd)
{
	return recording(s) && cmd != nullptr && cmd->get_device() == s.game;
}

void flush_group(State &s)
{
	if (s.group.draws == 0)
		return;
	std::fprintf(s.text, "    %u draw(s), %llu elements (largest %u, instances up to %u) VS %s PS %s vp %s srv [%s] vs-cb [%s]\n",
		s.group.draws, static_cast<unsigned long long>(s.group.elements), s.group.max_elements,
		s.group.max_instances, shader_name(s, s.group.vs).c_str(), shader_name(s, s.group.ps).c_str(),
		s.group.vp.c_str(), s.group.srv.c_str(), s.group.cb.c_str());
	s.group = Group{};
}

void write_raw(State &s, const wchar_t *name, const resource_desc &d, const subresource_data &m, uint32_t step)
{
	const uint32_t bpp = pipeline_trace_format_bytes(static_cast<uint32_t>(d.texture.format));
	const uint32_t w = (d.texture.width + step - 1) / step, h = (d.texture.height + step - 1) / step;
	FILE *f = nullptr;
	if (_wfopen_s(&f, (s.run_dir + L"\\" + name).c_str(), L"wb") != 0 || f == nullptr)
		return;
	const uint32_t header[6] = { 0x54504541u , w, h, static_cast<uint32_t>(d.texture.format), bpp, step };
	std::fwrite(header, sizeof(header), 1, f);
	std::vector<uint8_t> row(static_cast<size_t>(w) * bpp);
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t *src = static_cast<const uint8_t *>(m.data) + static_cast<size_t>(y) * step * m.row_pitch;
		for (uint32_t x = 0; x < w; ++x)
			std::memcpy(&row[static_cast<size_t>(x) * bpp], src + static_cast<size_t>(x) * step * bpp, bpp);
		std::fwrite(row.data(), row.size(), 1, f);
	}
	std::fclose(f);
}

void dump(State &s, command_list *cmd, resource r, const char *what)
{
	if (s.phase != 2 || s.dumps >= kMaxDumps || r.handle == 0 || cmd == nullptr)
		return;
	const device_api api = cmd->get_device()->get_api();
	if (api != device_api::d3d10 && api != device_api::d3d11)
		return;
	const resource_desc *const known = desc_of(s, r);
	if (known == nullptr || known->type != resource_type::texture_2d)
		return;
	const resource_desc d = *known;
	const uint32_t fmt = static_cast<uint32_t>(d.texture.format);
	if (pipeline_trace_format_bytes(fmt) == 0 || d.texture.samples > 1 || d.texture.depth_or_layers != 1)
		return;
	device *dev = cmd->get_device();
	resource_desc sd(d.texture.width, d.texture.height, 1, 1, d.texture.format, 1, memory_heap::readback,
		resource_usage::copy_dest);
	resource staging{};
	if (!dev->create_resource(sd, nullptr, resource_usage::copy_dest, &staging))
		return;
	if (d.texture.levels == 1)
		cmd->copy_resource(r, staging);
	else
		cmd->copy_texture_region(r, 0, nullptr, staging, 0, nullptr, filter_mode::min_mag_mip_point);
	subresource_data m{};
	if (dev->map_texture_region(staging, 0, nullptr, map_access::read_only, &m)) {
		const bool whole = pipeline_trace_two_channel(fmt) ||
			(d.usage & resource_usage::depth_stencil) != resource_usage::undefined || d.texture.width <= 1280;
		wchar_t name[96];
		_snwprintf_s(name, _TRUNCATE, L"p%03u_%hs_%hs.raw", s.pass, what, name_of(s, r).c_str());
		write_raw(s, name, d, m, whole ? 1u : 2u);
		dev->unmap_texture_region(staging, 0);
		++s.dumps;
	}
	dev->destroy_resource(staging);
}

void end_pass(State &s, command_list *cmd)
{
	if (!recording(s, cmd))
		return;
	flush_group(s);
	if (s.pass_wrote) {
		for (uint32_t i = 0; i < s.rt_count; ++i)
			dump(s, cmd, s.rts[i], "rt");
		dump(s, cmd, s.ds, "ds");
	}
	s.pass_wrote = false;
}

void begin_frame(State &s)
{
	s.pass = 0;
	s.pass_wrote = false;
	s.group = Group{};
	s.table.clear();
	wchar_t name[32];
	_snwprintf_s(name, _TRUNCATE, L"\\frame%u.txt", s.phase - 1);
	if (_wfopen_s(&s.text, (s.run_dir + name).c_str(), L"w") != 0)
		s.text = nullptr;
	if (s.text == nullptr)
		return;
	std::fprintf(s.text, "frame %u (passes in the order the game drew them)\n", s.phase - 1);
	std::string line;
	for (uint32_t i = 0; i < s.rt_count; ++i) {
		if (s.rts[i].handle == 0)
			continue;
		note_table(s, s.rts[i]);
		line += " RT" + std::to_string(i) + "=" + name_of(s, s.rts[i]) + "(" + size_of(s, s.rts[i]) + ")";
	}
	if (s.ds.handle != 0) {
		note_table(s, s.ds);
		line += " DS=" + name_of(s, s.ds) + "(" + size_of(s, s.ds) + ")";
	}
	std::fprintf(s.text, "P000 bound from the frame before:%s\n", line.empty() ? " nothing" : line.c_str());
}

void end_frame(State &s)
{
	if (s.text == nullptr)
		return;
	std::fprintf(s.text, "\nresources named in this frame:\n");
	for (const std::string &t : s.table)
		std::fprintf(s.text, "  %s\n", t.c_str());
	std::fclose(s.text);
	s.text = nullptr;
}

void write_shaders(State &s)
{
	for (const auto &u : s.used_shaders) {
		auto it = s.shaders.find(u.first);
		if (it == s.shaders.end() || it->second.code.empty())
			continue;
		wchar_t name[48];
		_snwprintf_s(name, _TRUNCATE, L"\\shaders\\%c%08x.dxbc", static_cast<wchar_t>(it->second.stage),
			it->second.hash);
		FILE *f = nullptr;
		if (_wfopen_s(&f, (s.run_dir + name).c_str(), L"wb") == 0 && f != nullptr) {
			std::fwrite(it->second.code.data(), it->second.code.size(), 1, f);
			std::fclose(f);
		}
	}
	s.used_shaders.clear();
}

void arm(State &s)
{
	SYSTEMTIME t{};
	GetLocalTime(&t);
	wchar_t name[64];
	_snwprintf_s(name, _TRUNCATE, L"\\pipeline-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour,
		t.wMinute, t.wSecond);
	s.run_dir = s.dir + name;
	CreateDirectoryW(s.dir.c_str(), nullptr);
	CreateDirectoryW(s.run_dir.c_str(), nullptr);
	CreateDirectoryW((s.run_dir + L"\\shaders").c_str(), nullptr);
	s.phase = 1;
	s.dumps = 0;
	diag_info("trace", L"pipeline trace: the next " + std::to_wstring(kTextFrames) + L" frames into " + s.run_dir);
}

void on_init_resource(device *, const resource_desc &desc, const subresource_data *, resource_usage, resource r)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	s.resources[r.handle] = desc;
}

void on_destroy_resource(device *, resource r)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	s.resources.erase(r.handle);
	s.ids.erase(r.handle);
}

void on_init_pipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subs, pipeline p)
{
	for (uint32_t i = 0; i < count; ++i) {
		char stage = 0;
		switch (subs[i].type) {
		case pipeline_subobject_type::vertex_shader: stage = 'V'; break;
		case pipeline_subobject_type::pixel_shader: stage = 'P'; break;
		case pipeline_subobject_type::compute_shader: stage = 'C'; break;
		case pipeline_subobject_type::geometry_shader: stage = 'G'; break;
		case pipeline_subobject_type::hull_shader: stage = 'H'; break;
		case pipeline_subobject_type::domain_shader: stage = 'D'; break;
		default: continue;
		}
		const shader_desc *sd = static_cast<const shader_desc *>(subs[i].data);
		if (sd == nullptr || sd->code == nullptr || sd->code_size == 0)
			continue;
		ShaderInfo info;
		info.stage = static_cast<uint32_t>(stage);
		info.hash = fnv1a(sd->code, sd->code_size);
		info.code.assign(static_cast<const uint8_t *>(sd->code), static_cast<const uint8_t *>(sd->code) + sd->code_size);
		State &s = st();
		std::lock_guard<std::mutex> g(s.lock);
		s.shaders[p.handle] = std::move(info);
		return;
	}
}

void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline p)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (cmd->get_device() != s.game)
		return;
	if ((stages & pipeline_stage::vertex_shader) != static_cast<pipeline_stage>(0)) s.vs = p.handle;
	if ((stages & pipeline_stage::pixel_shader) != static_cast<pipeline_stage>(0)) s.ps = p.handle;
	if ((stages & pipeline_stage::compute_shader) != static_cast<pipeline_stage>(0)) s.cs = p.handle;
}

void on_bind_render_targets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (cmd->get_device() != s.game && s.game != nullptr)
		return;
	if (!recording(s)) {
		device *dev = cmd->get_device();
		s.rt_count = count < 8 ? count : 8;
		for (uint32_t i = 0; i < s.rt_count; ++i)
			s.rts[i] = rtvs[i].handle != 0 ? dev->get_resource_from_view(rtvs[i]) : resource{};
		s.ds = dsv.handle != 0 ? dev->get_resource_from_view(dsv) : resource{};
		return;
	}
	end_pass(s, cmd);
	device *dev = cmd->get_device();
	s.rt_count = count < 8 ? count : 8;
	for (uint32_t i = 0; i < s.rt_count; ++i)
		s.rts[i] = rtvs[i].handle != 0 ? dev->get_resource_from_view(rtvs[i]) : resource{};
	s.ds = dsv.handle != 0 ? dev->get_resource_from_view(dsv) : resource{};
	++s.pass;
	std::string line;
	for (uint32_t i = 0; i < s.rt_count; ++i) {
		if (s.rts[i].handle == 0)
			continue;
		note_table(s, s.rts[i]);
		line += " RT" + std::to_string(i) + "=" + name_of(s, s.rts[i]) + "(" + size_of(s, s.rts[i]) + ")";
	}
	if (s.ds.handle != 0) {
		note_table(s, s.ds);
		line += " DS=" + name_of(s, s.ds) + "(" + size_of(s, s.ds) + ")";
	}
	std::fprintf(s.text, "P%03u%s\n", s.pass, line.empty() ? " nothing bound" : line.c_str());
}

void on_bind_viewports(command_list *cmd, uint32_t first, uint32_t count, const viewport *vps)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (cmd->get_device() != s.game)
		return;
	if (first == 0 && count > 0)
		s.vp = vps[0];
}

void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update &u)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (cmd->get_device() != s.game)
		return;
	device *dev = cmd->get_device();
	if (u.type == descriptor_type::shader_resource_view || u.type == descriptor_type::sampler_with_resource_view) {
		for (uint32_t i = 0; i < u.count && u.binding + i < 16; ++i) {
			resource_view v = u.type == descriptor_type::shader_resource_view
				? static_cast<const resource_view *>(u.descriptors)[i]
				: static_cast<const sampler_with_resource_view *>(u.descriptors)[i].view;
			const resource r = v.handle != 0 ? dev->get_resource_from_view(v) : resource{};
			if ((stages & shader_stage::pixel) != static_cast<shader_stage>(0))
				s.ps_srv[u.binding + i] = r;
			if ((stages & shader_stage::compute) != static_cast<shader_stage>(0))
				s.cs_srv[u.binding + i] = r;
		}
	} else if (u.type == descriptor_type::constant_buffer && (stages & shader_stage::vertex) != static_cast<shader_stage>(0)) {
		for (uint32_t i = 0; i < u.count && u.binding + i < 16; ++i) {
			const buffer_range &b = static_cast<const buffer_range *>(u.descriptors)[i];
			uint64_t size = b.size;
			if (size == UINT64_MAX || size == 0) {
				auto it = s.resources.find(b.buffer.handle);
				size = it != s.resources.end() ? it->second.buffer.size : 0;
			}
			s.vs_cb_size[u.binding + i] = b.buffer.handle != 0 ? static_cast<uint32_t>(size) : 0u;
		}
	}
}

void on_draw_common(command_list *cmd, uint32_t elements, uint32_t instances)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (!recording(s, cmd))
		return;
	std::string srv;
	for (uint32_t i = 0; i < 16; ++i) {
		if (s.ps_srv[i].handle == 0)
			continue;
		note_table(s, s.ps_srv[i]);
		srv += "t" + std::to_string(i) + "=" + name_of(s, s.ps_srv[i]) + " ";
	}
	std::string cb;
	for (uint32_t i = 0; i < 16; ++i) {
		if (s.vs_cb_size[i] != 0)
			cb += "b" + std::to_string(i) + ":" + std::to_string(s.vs_cb_size[i]) + " ";
	}
	char vp[64];
	std::snprintf(vp, sizeof(vp), "%.0f,%.0f %.0fx%.0f", s.vp.x, s.vp.y, s.vp.width, s.vp.height);
	if (s.group.draws == 0 || s.group.vs != s.vs || s.group.ps != s.ps || s.group.srv != srv || s.group.cb != cb ||
		s.group.vp != vp) {
		flush_group(s);
		s.group.vs = s.vs;
		s.group.ps = s.ps;
		s.group.srv = srv;
		s.group.cb = cb;
		s.group.vp = vp;
	}
	++s.group.draws;
	s.group.elements += elements;
	if (elements > s.group.max_elements)
		s.group.max_elements = elements;
	if (instances > s.group.max_instances)
		s.group.max_instances = instances;
	s.pass_wrote = true;
}

bool on_draw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t)
{
	on_draw_common(cmd, vertices, instances);
	return false;
}

bool on_draw_indexed(command_list *cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t)
{
	on_draw_common(cmd, indices, instances);
	return false;
}

bool on_dispatch(command_list *cmd, uint32_t x, uint32_t y, uint32_t z)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (!recording(s, cmd))
		return false;
	flush_group(s);
	std::string srv;
	for (uint32_t i = 0; i < 16; ++i) {
		if (s.cs_srv[i].handle != 0) {
			note_table(s, s.cs_srv[i]);
			srv += "t" + std::to_string(i) + "=" + name_of(s, s.cs_srv[i]) + " ";
		}
	}
	std::fprintf(s.text, "    dispatch %ux%ux%u CS %s srv [%s]\n", x, y, z, shader_name(s, s.cs).c_str(), srv.c_str());
	return false;
}

bool on_clear_rtv(command_list *cmd, resource_view rtv, const float c[4], uint32_t rects, const rect *)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (!recording(s, cmd))
		return false;
	flush_group(s);
	const resource r = cmd->get_device()->get_resource_from_view(rtv);
	note_table(s, r);
	std::fprintf(s.text, "    clear %s to (%.3g %.3g %.3g %.3g)%s\n", name_of(s, r).c_str(), c[0], c[1], c[2], c[3],
		rects != 0 ? " in rectangles" : "");
	s.pass_wrote = true;
	return false;
}

bool on_clear_dsv(command_list *cmd, resource_view dsv, const float *depth, const uint8_t *stencil, uint32_t, const rect *)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (!recording(s, cmd))
		return false;
	flush_group(s);
	const resource r = cmd->get_device()->get_resource_from_view(dsv);
	note_table(s, r);
	std::fprintf(s.text, "    clear depth %s%s%s\n", name_of(s, r).c_str(),
		depth != nullptr ? (" to " + std::to_string(*depth)).c_str() : "",
		stencil != nullptr ? (" stencil " + std::to_string(*stencil)).c_str() : "");
	return false;
}

bool on_copy_resource(command_list *cmd, resource src, resource dst)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (!recording(s, cmd))
		return false;
	end_pass(s, cmd);
	note_table(s, src);
	note_table(s, dst);
	std::fprintf(s.text, "    copy %s(%s) -> %s(%s)\n", name_of(s, src).c_str(), size_of(s, src).c_str(),
		name_of(s, dst).c_str(), size_of(s, dst).c_str());
	return false;
}

bool on_copy_texture_region(command_list *cmd, resource src, uint32_t ssub, const subresource_box *sbox, resource dst,
	uint32_t dsub, const subresource_box *dbox, filter_mode)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (!recording(s, cmd))
		return false;
	end_pass(s, cmd);
	note_table(s, src);
	note_table(s, dst);
	char box[96] = "";
	if (sbox != nullptr)
		std::snprintf(box, sizeof(box), " box %d,%d-%d,%d", sbox->left, sbox->top, sbox->right, sbox->bottom);
	std::fprintf(s.text, "    copy region %s[%u]%s -> %s[%u]%s\n", name_of(s, src).c_str(), ssub, box,
		name_of(s, dst).c_str(), dsub, dbox != nullptr ? " (placed)" : "");
	return false;
}

bool on_resolve(command_list *cmd, resource src, uint32_t, const subresource_box *, resource dst, uint32_t, uint32_t,
	uint32_t, uint32_t, format fmt)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (!recording(s, cmd))
		return false;
	end_pass(s, cmd);
	note_table(s, src);
	note_table(s, dst);
	std::fprintf(s.text, "    resolve %s -> %s as %s\n", name_of(s, src).c_str(), name_of(s, dst).c_str(),
		describe_format(static_cast<uint32_t>(fmt)).c_str());
	return false;
}

void on_present(command_queue *queue, swapchain *chain, const rect *, const rect *, uint32_t, const rect *)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (chain != nullptr && s.phase == 0)
		s.game = chain->get_device();
	if (s.phase >= 2) {
		command_list *cmd = queue != nullptr ? queue->get_immediate_command_list() : nullptr;
		if (s.text != nullptr) {
			end_pass(s, cmd);
			const resource bb = current_back_buffer_of(chain);
			note_table(s, bb);
			std::fprintf(s.text, "present %s(%s)\n", name_of(s, bb).c_str(), size_of(s, bb).c_str());
			dump(s, cmd, bb, "present");
		}
		end_frame(s);
		write_shaders(s);
		if (s.phase - 1 >= kTextFrames) {
			s.phase = 0;
			diag_info("trace", L"pipeline trace written: " + s.run_dir);
			return;
		}
		++s.phase;
		begin_frame(s);
		return;
	}
	if (s.phase == 1) {
		s.phase = 2;
		begin_frame(s);
	}
}

void on_reshade_present(effect_runtime *runtime)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (s.phase == 0 && runtime != nullptr && runtime->is_key_pressed(s.key))
		arm(s);
}

bool g_registered = false;

}

void register_pipeline_trace(unsigned int key, const std::wstring &dir)
{
	State &s = st();
	{
		std::lock_guard<std::mutex> g(s.lock);
		s.key = key != 0 ? key : 0x76u;
		s.dir = dir;
	}
	if (g_registered)
		return;
	g_registered = true;
	reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
	reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
	reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
	reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
	reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
	reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
	reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	reshade::register_event<reshade::addon_event::draw>(on_draw);
	reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
	reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
	reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_rtv);
	reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_dsv);
	reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
	reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
	reshade::register_event<reshade::addon_event::resolve_texture_region>(on_resolve);
	reshade::register_event<reshade::addon_event::present>(on_present);
	reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
	diag_info("trace", L"pipeline trace: on, the key writes the next frames' passes into " + dir);
}

void unregister_pipeline_trace()
{
	if (!g_registered)
		return;
	g_registered = false;
	reshade::unregister_event<reshade::addon_event::init_resource>(on_init_resource);
	reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
	reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
	reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
	reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
	reshade::unregister_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
	reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	reshade::unregister_event<reshade::addon_event::draw>(on_draw);
	reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
	reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
	reshade::unregister_event<reshade::addon_event::clear_render_target_view>(on_clear_rtv);
	reshade::unregister_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_dsv);
	reshade::unregister_event<reshade::addon_event::copy_resource>(on_copy_resource);
	reshade::unregister_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
	reshade::unregister_event<reshade::addon_event::resolve_texture_region>(on_resolve);
	reshade::unregister_event<reshade::addon_event::present>(on_present);
	reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
}

}

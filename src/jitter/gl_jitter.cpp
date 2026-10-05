#include "aeon_sr/jitter/gl_jitter.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace aeon_sr {
namespace {

typedef PROC(WINAPI *PfnWglGetProcAddress)(LPCSTR);
typedef HGLRC(WINAPI *PfnWglGetCurrentContext)(void);
typedef void(WINAPI *PfnGetIntegerv)(unsigned int, int *);
typedef void(WINAPI *PfnViewportIndexedf)(unsigned int, float, float, float, float);
typedef void(WINAPI *PfnGetFloati)(unsigned int, unsigned int, float *);
typedef const unsigned char *(WINAPI *PfnGetString)(unsigned int);

constexpr unsigned int kGlVersion = 0x1F02u;
constexpr unsigned int kGlViewport = 0x0BA2u;
constexpr unsigned int kGlViewportSubpixelBits = 0x825Cu;
constexpr unsigned int kGlClipOrigin = 0x935Cu;
constexpr int kGlUpperLeft = 0x8CA2;

struct GlEntry {
	HMODULE gl32 = nullptr;
	PfnWglGetProcAddress get_proc = nullptr;
	PfnWglGetCurrentContext current = nullptr;
	PfnGetIntegerv get_integerv = nullptr;
	PfnGetString get_string = nullptr;
};

struct GlContext {
	HGLRC context = nullptr;
	PfnViewportIndexedf viewport_indexed = nullptr;
	PfnGetFloati get_floati = nullptr;
	bool clip_control = false;
	int bits = 0;
	const char *note = "";
	uint64_t observed = ~0ull;
};

std::atomic<bool> g_upper_left{ false };

std::once_flag g_once;
GlEntry g_entry;
thread_local GlContext t_ctx;
thread_local bool t_own = false;

PROC proc_of(PfnWglGetProcAddress get, const char *name)
{
	const PROC p = get != nullptr ? get(name) : nullptr;
	const uintptr_t v = reinterpret_cast<uintptr_t>(p);
	return (v <= 3 || v == static_cast<uintptr_t>(-1)) ? nullptr : p;
}

const GlEntry &entry() noexcept
{
	std::call_once(g_once, [] {
		g_entry.gl32 = GetModuleHandleW(L"opengl32.dll");
		if (g_entry.gl32 == nullptr)
			return;
		g_entry.get_proc = reinterpret_cast<PfnWglGetProcAddress>(GetProcAddress(g_entry.gl32, "wglGetProcAddress"));
		g_entry.current = reinterpret_cast<PfnWglGetCurrentContext>(GetProcAddress(g_entry.gl32, "wglGetCurrentContext"));
		g_entry.get_integerv = reinterpret_cast<PfnGetIntegerv>(GetProcAddress(g_entry.gl32, "glGetIntegerv"));
		g_entry.get_string = reinterpret_cast<PfnGetString>(GetProcAddress(g_entry.gl32, "glGetString"));
	});
	return g_entry;
}

const GlContext *resolved() noexcept
{
	const GlEntry &e = entry();
	if (e.get_proc == nullptr || e.current == nullptr || e.get_integerv == nullptr || e.get_string == nullptr)
		return nullptr;
	const HGLRC ctx = e.current();
	if (ctx == nullptr)
		return nullptr;
	GlContext &c = t_ctx;
	if (ctx != c.context) {
		c = GlContext{};
		c.context = ctx;
		int major = 0, minor = 0;
		if (const unsigned char *v = e.get_string(kGlVersion)) {
			const char *text = reinterpret_cast<const char *>(v);
			major = std::atoi(text);
			if (const char *dot = std::strchr(text, 46))
				minor = std::atoi(dot + 1);
		}
		const int version = major * 10 + minor;
		if (version >= 41) {
			c.viewport_indexed = reinterpret_cast<PfnViewportIndexedf>(proc_of(e.get_proc, "glViewportIndexedf"));
			c.get_floati = reinterpret_cast<PfnGetFloati>(proc_of(e.get_proc, "glGetFloati_v"));
			if (c.get_floati == nullptr)
				c.viewport_indexed = nullptr;
		}
		c.clip_control = version >= 45;
		if (c.viewport_indexed != nullptr)
			e.get_integerv(kGlViewportSubpixelBits, &c.bits);
		c.note = c.viewport_indexed == nullptr
			? "this OpenGL driver has no fractional viewports (OpenGL 4.1)"
			: c.bits < kGlJitterMinSubpixelBits
			? "this OpenGL driver rounds viewports to whole pixels"
			: "";
	}
	return &c;
}

}

bool gl_jitter_usable() noexcept
{
	const GlContext *const g = resolved();
	return g != nullptr && g->viewport_indexed != nullptr && g->bits >= kGlJitterMinSubpixelBits;
}

const char *gl_jitter_note() noexcept
{
	const GlContext *const g = resolved();
	return g == nullptr ? "no OpenGL context is current" : g->note;
}

int gl_jitter_subpixel_bits() noexcept
{
	const GlContext *const g = resolved();
	return g != nullptr ? g->bits : 0;
}

bool gl_jitter_bind(uint32_t count, const JitterViewport *viewports) noexcept
{
	const GlContext *const g = resolved();
	if (g == nullptr || g->viewport_indexed == nullptr || viewports == nullptr)
		return false;
	const bool was = t_own;
	t_own = true;
	for (uint32_t i = 0; i < count; ++i)
		g->viewport_indexed(i, viewports[i].x, viewports[i].y, viewports[i].width, viewports[i].height);
	t_own = was;
	return true;
}

bool gl_jitter_own_call() noexcept
{
	return t_own;
}

uint32_t gl_jitter_read(uint32_t count, JitterViewport *out) noexcept
{
	const GlContext *const g = resolved();
	if (g == nullptr || g->get_floati == nullptr || out == nullptr)
		return 0;
	for (uint32_t i = 0; i < count; ++i) {
		float v[4]{};
		g->get_floati(kGlViewport, i, v);
		out[i].x = v[0];
		out[i].y = v[1];
		out[i].width = v[2];
		out[i].height = v[3];
	}
	return count;
}

bool gl_jitter_bound_as(uint32_t count, const JitterViewport *expect) noexcept
{
	const GlContext *const g = resolved();
	if (g == nullptr || g->get_floati == nullptr || expect == nullptr)
		return false;
	const int bits = g->bits > 0 && g->bits < 16 ? g->bits : 8;
	const float step = 1.0f / static_cast<float>(1 << bits) + 1e-4f;
	for (uint32_t i = 0; i < count; ++i) {
		float v[4]{};
		g->get_floati(kGlViewport, i, v);
		if (std::fabs(v[0] - expect[i].x) > step || std::fabs(v[1] - expect[i].y) > step ||
			std::fabs(v[2] - expect[i].width) > 1e-3f || std::fabs(v[3] - expect[i].height) > 1e-3f)
			return false;
	}
	return true;
}

void gl_jitter_observe(uint64_t frame) noexcept
{
	GlContext *const g = const_cast<GlContext *>(resolved());
	if (g == nullptr || g->observed == frame)
		return;
	g->observed = frame;
	if (!g->clip_control)
		return;
	int origin = 0;
	entry().get_integerv(kGlClipOrigin, &origin);
	g_upper_left.store(origin == kGlUpperLeft);
}

bool gl_clip_origin_upper_left() noexcept
{
	return g_upper_left.load();
}

GlJitterQuiet::GlJitterQuiet() noexcept : was_(t_own)
{
	t_own = true;
}

GlJitterQuiet::~GlJitterQuiet()
{
	t_own = was_;
}

}

#include "aeon_sr/interop/d3d9ex_upgrade.hpp"

#include <mutex>
#include <vector>

namespace aeon_sr {
namespace d3d9ex {

namespace {

IDirect3D9Ex *g_upgraded_ex = nullptr;
UINT g_upgraded_adapter = 0;

std::vector<D3DDISPLAYMODEEX> modes_of(IDirect3D9Ex *self, UINT adapter, D3DFORMAT format)
{
	std::vector<D3DDISPLAYMODEEX> out;
	if (self == nullptr)
		return out;
	D3DDISPLAYMODEFILTER filter{};
	filter.Size = sizeof(filter);
	filter.Format = format;
	filter.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
	const UINT n = self->GetAdapterModeCountEx(adapter, &filter);
	out.reserve(n);
	for (UINT i = 0; i < n; ++i) {
		D3DDISPLAYMODEEX m{};
		m.Size = sizeof(m);
		if (SUCCEEDED(self->EnumAdapterModesEx(adapter, &filter, i, &m)))
			out.push_back(m);
	}
	return out;
}

constexpr size_t kCreateDeviceExSlot = 20;

std::mutex g_lock;
HMODULE g_system = nullptr;
void **g_vtable = nullptr;
CreateDeviceExFn g_original = nullptr;
Result g_result;
thread_local bool t_armed = false;
uint32_t g_armed = 0;

HRESULT STDMETHODCALLTYPE create_device_ex(IDirect3D9Ex *self, UINT adapter, D3DDEVTYPE type, HWND focus,
	DWORD flags, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode, IDirect3DDevice9Ex **out);

void unpatch_locked()
{
	if (g_vtable == nullptr || g_original == nullptr)
		return;
	void **const slot = &g_vtable[kCreateDeviceExSlot];
	if (*slot != reinterpret_cast<void *>(&create_device_ex))
		return;
	DWORD old = 0;
	if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
		return;
	*slot = reinterpret_cast<void *>(g_original);
	VirtualProtect(slot, sizeof(void *), old, &old);
}

HRESULT create_plain(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp,
	IDirect3DDevice9 **out)
{
	using Create9 = IDirect3D9 *(WINAPI *)(UINT);
	const auto create = g_system != nullptr ? reinterpret_cast<Create9>(GetProcAddress(g_system, "Direct3DCreate9"))
		: nullptr;
	IDirect3D9 *const d3d = create != nullptr ? create(D3D_SDK_VERSION) : nullptr;
	if (d3d == nullptr)
		return D3DERR_NOTAVAILABLE;
	const HRESULT hr = d3d->CreateDevice(adapter, type, focus, flags, pp, out);
	d3d->Release();
	return hr;
}

HRESULT STDMETHODCALLTYPE create_device_ex(IDirect3D9Ex *self, UINT adapter, D3DDEVTYPE type, HWND focus,
	DWORD flags, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode, IDirect3DDevice9Ex **out)
{
	CreateDeviceExFn original = nullptr;
	const bool mine = t_armed;
	{
		const std::lock_guard<std::mutex> lock(g_lock);
		original = g_original;
		if (mine) {
			t_armed = false;
			if (g_armed != 0 && --g_armed == 0)
				unpatch_locked();
		}
	}
	if (!mine)
		return original(self, adapter, type, focus, flags, pp, mode, out);
	Result r;
	const HRESULT hr = upgrade_create(original, &create_plain, self, adapter, type, focus, flags, pp, mode, out, &r);
	const std::lock_guard<std::mutex> lock(g_lock);
	g_result = r;
	return hr;
}

}

D3DFORMAT display_format_of(D3DFORMAT back_buffer) noexcept
{
	switch (back_buffer) {
	case D3DFMT_A8R8G8B8: return D3DFMT_X8R8G8B8;
	case D3DFMT_A1R5G5B5: return D3DFMT_X1R5G5B5;
	default: return back_buffer;
	}
}

D3DDISPLAYMODEEX fullscreen_mode_of(const D3DPRESENT_PARAMETERS &pp, const D3DDISPLAYMODEEX &desktop) noexcept
{
	D3DDISPLAYMODEEX m{};
	m.Size = sizeof(m);
	const bool sized = pp.BackBufferWidth != 0 && pp.BackBufferHeight != 0;
	m.Width = sized ? pp.BackBufferWidth : desktop.Width;
	m.Height = sized ? pp.BackBufferHeight : desktop.Height;
	m.RefreshRate = pp.FullScreen_RefreshRateInHz;
	m.Format = pp.BackBufferFormat != D3DFMT_UNKNOWN ? display_format_of(pp.BackBufferFormat) : desktop.Format;
	m.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
	return m;
}

UINT refresh_rate_for(UINT width, UINT height, const D3DDISPLAYMODEEX *modes, size_t count,
	UINT desktop_rate) noexcept
{
	UINT highest = 0;
	for (size_t i = 0; modes != nullptr && i < count; ++i) {
		if (modes[i].Width != width || modes[i].Height != height)
			continue;
		if (desktop_rate != 0 && modes[i].RefreshRate == desktop_rate)
			return desktop_rate;
		if (modes[i].RefreshRate > highest)
			highest = modes[i].RefreshRate;
	}
	return highest;
}

HRESULT upgrade_create(CreateDeviceExFn original, CreatePlainFn plain, IDirect3D9Ex *self, UINT adapter,
	D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode,
	IDirect3DDevice9Ex **out, Result *result, const ModeList *modes)
{
	Result r;
	const D3DPRESENT_PARAMETERS asked = pp != nullptr ? *pp : D3DPRESENT_PARAMETERS{};
	r.fullscreen = pp != nullptr && !pp->Windowed;
	D3DDISPLAYMODEEX tries[kMaxAttempts]{};
	int try_count = 0;
	if (r.fullscreen && mode == nullptr) {
		D3DDISPLAYMODEEX desktop{};
		desktop.Size = sizeof(desktop);
		if (modes != nullptr && modes->desktop != nullptr) {
			desktop = *modes->desktop;
		} else if (self == nullptr || FAILED(self->GetAdapterDisplayModeEx(adapter, &desktop, nullptr))) {
			desktop = D3DDISPLAYMODEEX{};
			desktop.Size = sizeof(desktop);
		}
		D3DDISPLAYMODEEX made = fullscreen_mode_of(*pp, desktop);
		if (made.RefreshRate == 0) {
			const std::vector<D3DDISPLAYMODEEX> read = modes == nullptr ? modes_of(self, adapter, made.Format)
				: std::vector<D3DDISPLAYMODEEX>{};
			const D3DDISPLAYMODEEX *list = modes != nullptr ? modes->modes : read.data();
			const size_t count = modes != nullptr ? modes->count : read.size();
			const UINT rate = refresh_rate_for(made.Width, made.Height, list, count, desktop.RefreshRate);
			if (rate != 0) {
				made.RefreshRate = rate;
				r.refresh_filled = rate;
			}
		}
		const auto add = [&](D3DDISPLAYMODEEX m) {
			for (int i = 0; i < try_count; ++i)
				if (tries[i].Width == m.Width && tries[i].Height == m.Height && tries[i].RefreshRate == m.RefreshRate &&
					tries[i].Format == m.Format && tries[i].ScanLineOrdering == m.ScanLineOrdering)
					return;
			if (try_count < kMaxAttempts)
				tries[try_count++] = m;
		};
		D3DDISPLAYMODEEX own = made;
		if (asked.BackBufferFormat != D3DFMT_UNKNOWN)
			own.Format = asked.BackBufferFormat;
		add(own);
		add(made);
		D3DDISPLAYMODEEX open = made;
		open.ScanLineOrdering = D3DSCANLINEORDERING_UNKNOWN;
		add(open);
		own.ScanLineOrdering = D3DSCANLINEORDERING_UNKNOWN;
		add(own);
		if (desktop.Width == made.Width && desktop.Height == made.Height && desktop.RefreshRate != 0)
			add(desktop);
		r.mode_made = true;
	}
	HRESULT hr = D3DERR_NOTAVAILABLE;
	for (int i = 0; i < (try_count > 0 ? try_count : 1); ++i) {
		D3DDISPLAYMODEEX *const ask = try_count > 0 ? &tries[i] : mode;
		if (try_count > 0) {
			*pp = asked;
			pp->FullScreen_RefreshRateInHz = ask->RefreshRate;
		}
		if (out != nullptr)
			*out = nullptr;
		hr = original != nullptr ? original(self, adapter, type, focus, flags, pp, ask, out) : D3DERR_NOTAVAILABLE;
		if (try_count > 0 && r.attempt_count < kMaxAttempts) {
			Attempt &a = r.attempts[r.attempt_count++];
			a.width = ask->Width;
			a.height = ask->Height;
			a.refresh = ask->RefreshRate;
			a.format = ask->Format;
			a.scanline = ask->ScanLineOrdering;
			a.hr = hr;
		}
		if (SUCCEEDED(hr))
			break;
	}
	r.ex_hr = hr;
	if (SUCCEEDED(hr)) {
		r.outcome = Outcome::Upgraded;
		if (self != nullptr) {
			const std::lock_guard<std::mutex> lock(g_lock);
			if (g_upgraded_ex != self) {
				self->AddRef();
				if (g_upgraded_ex != nullptr)
					g_upgraded_ex->Release();
				g_upgraded_ex = self;
			}
			g_upgraded_adapter = adapter;
		}
	} else {
		if (pp != nullptr)
			*pp = asked;
		if (out != nullptr)
			*out = nullptr;
		hr = plain(adapter, type, focus, flags, pp, reinterpret_cast<IDirect3DDevice9 **>(out));
		r.outcome = Outcome::FellBack;
	}
	if (result != nullptr)
		*result = r;
	return hr;
}

UINT refresh_for_reset(UINT width, UINT height, D3DFORMAT back_buffer_format)
{
	const std::lock_guard<std::mutex> lock(g_lock);
	if (g_upgraded_ex == nullptr)
		return 0;
	D3DDISPLAYMODEEX desktop{};
	desktop.Size = sizeof(desktop);
	if (FAILED(g_upgraded_ex->GetAdapterDisplayModeEx(g_upgraded_adapter, &desktop, nullptr)))
		return 0;
	if (width == 0 || height == 0) {
		width = desktop.Width;
		height = desktop.Height;
	}
	const D3DFORMAT format = back_buffer_format != D3DFMT_UNKNOWN ? display_format_of(back_buffer_format)
		: desktop.Format;
	const std::vector<D3DDISPLAYMODEEX> modes = modes_of(g_upgraded_ex, g_upgraded_adapter, format);
	return refresh_rate_for(width, height, modes.data(), modes.size(), desktop.RefreshRate);
}

bool arm()
{
	const std::lock_guard<std::mutex> lock(g_lock);
	if (g_system == nullptr) {
		wchar_t path[MAX_PATH];
		const UINT n = GetSystemDirectoryW(path, MAX_PATH);
		if (n == 0 || n + 10 >= MAX_PATH)
			return false;
		wcscat_s(path, L"\\d3d9.dll");
		g_system = LoadLibraryW(path);
	}
	using CreateEx = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);
	const auto create_ex = g_system != nullptr
		? reinterpret_cast<CreateEx>(GetProcAddress(g_system, "Direct3DCreate9Ex")) : nullptr;
	IDirect3D9Ex *ex = nullptr;
	if (create_ex == nullptr || FAILED(create_ex(D3D_SDK_VERSION, &ex)) || ex == nullptr)
		return false;
	if (g_vtable == nullptr)
		g_vtable = *reinterpret_cast<void ***>(ex);
	const bool same_table = *reinterpret_cast<void ***>(ex) == g_vtable;
	ex->Release();
	if (!same_table)
		return false;
	void **const slot = &g_vtable[kCreateDeviceExSlot];
	if (*slot != reinterpret_cast<void *>(&create_device_ex)) {
		DWORD old = 0;
		if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
			return false;
		g_original = reinterpret_cast<CreateDeviceExFn>(*slot);
		*slot = reinterpret_cast<void *>(&create_device_ex);
		VirtualProtect(slot, sizeof(void *), old, &old);
	}
	if (!t_armed) {
		t_armed = true;
		++g_armed;
	}
	return true;
}

void disarm()
{
	const std::lock_guard<std::mutex> lock(g_lock);
	if (!t_armed)
		return;
	t_armed = false;
	if (g_armed != 0 && --g_armed == 0)
		unpatch_locked();
}

Result take_result()
{
	const std::lock_guard<std::mutex> lock(g_lock);
	const Result r = g_result;
	g_result = Result{};
	return r;
}

}
}

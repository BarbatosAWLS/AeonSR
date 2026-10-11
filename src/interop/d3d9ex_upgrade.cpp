#include "aeon_sr/interop/d3d9ex_upgrade.hpp"

#include <mutex>

namespace aeon_sr {
namespace d3d9ex {

namespace {

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

HRESULT upgrade_create(CreateDeviceExFn original, CreatePlainFn plain, IDirect3D9Ex *self, UINT adapter,
	D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode,
	IDirect3DDevice9Ex **out, Result *result)
{
	Result r;
	D3DDISPLAYMODEEX made{};
	r.fullscreen = pp != nullptr && !pp->Windowed;
	if (r.fullscreen && mode == nullptr) {
		D3DDISPLAYMODEEX desktop{};
		desktop.Size = sizeof(desktop);
		if (self == nullptr || FAILED(self->GetAdapterDisplayModeEx(adapter, &desktop, nullptr))) {
			desktop = D3DDISPLAYMODEEX{};
			desktop.Size = sizeof(desktop);
		}
		made = fullscreen_mode_of(*pp, desktop);
		mode = &made;
		r.mode_made = true;
	}
	const D3DPRESENT_PARAMETERS asked = pp != nullptr ? *pp : D3DPRESENT_PARAMETERS{};
	HRESULT hr = original != nullptr ? original(self, adapter, type, focus, flags, pp, mode, out)
		: D3DERR_NOTAVAILABLE;
	r.ex_hr = hr;
	if (SUCCEEDED(hr)) {
		r.outcome = Outcome::Upgraded;
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

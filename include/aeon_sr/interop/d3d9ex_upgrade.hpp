#pragma once

#include <Windows.h>
#include <d3d9.h>

namespace aeon_sr {
namespace d3d9ex {

D3DFORMAT display_format_of(D3DFORMAT back_buffer) noexcept;

D3DDISPLAYMODEEX fullscreen_mode_of(const D3DPRESENT_PARAMETERS &pp, const D3DDISPLAYMODEEX &desktop) noexcept;

using CreateDeviceExFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD,
	D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *, IDirect3DDevice9Ex **);
using CreatePlainFn = HRESULT (*)(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);

enum class Outcome { None, Upgraded, FellBack };
struct Result {
	Outcome outcome = Outcome::None;
	HRESULT ex_hr = S_OK;
	bool fullscreen = false;
	bool mode_made = false;
};

HRESULT upgrade_create(CreateDeviceExFn original, CreatePlainFn plain, IDirect3D9Ex *self, UINT adapter,
	D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode,
	IDirect3DDevice9Ex **out, Result *result);

bool arm();
void disarm();
Result take_result();

}
}

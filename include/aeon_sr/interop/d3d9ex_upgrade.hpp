#pragma once

#include <Windows.h>
#include <d3d9.h>

namespace aeon_sr {
namespace d3d9ex {

D3DFORMAT display_format_of(D3DFORMAT back_buffer) noexcept;

D3DDISPLAYMODEEX fullscreen_mode_of(const D3DPRESENT_PARAMETERS &pp, const D3DDISPLAYMODEEX &desktop) noexcept;

UINT refresh_rate_for(UINT width, UINT height, const D3DDISPLAYMODEEX *modes, size_t count,
	UINT desktop_rate) noexcept;

struct ModeList {
	const D3DDISPLAYMODEEX *modes = nullptr;
	size_t count = 0;
	const D3DDISPLAYMODEEX *desktop = nullptr;
};

struct Attempt {
	UINT width = 0, height = 0, refresh = 0;
	D3DFORMAT format = D3DFMT_UNKNOWN;
	D3DSCANLINEORDERING scanline = D3DSCANLINEORDERING_UNKNOWN;
	HRESULT hr = S_OK;
};
constexpr int kMaxAttempts = 6;

using CreateDeviceExFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD,
	D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *, IDirect3DDevice9Ex **);
using CreatePlainFn = HRESULT (*)(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);

enum class Outcome { None, Upgraded, FellBack };
struct Result {
	Outcome outcome = Outcome::None;
	HRESULT ex_hr = S_OK;
	bool fullscreen = false;
	bool mode_made = false;
	UINT refresh_filled = 0;
	Attempt attempts[kMaxAttempts]{};
	int attempt_count = 0;
};

HRESULT upgrade_create(CreateDeviceExFn original, CreatePlainFn plain, IDirect3D9Ex *self, UINT adapter,
	D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode,
	IDirect3DDevice9Ex **out, Result *result, const ModeList *modes = nullptr);

UINT refresh_for_reset(UINT width, UINT height, D3DFORMAT back_buffer_format);

bool arm();
void disarm();
Result take_result();

}
}

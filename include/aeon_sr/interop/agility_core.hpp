// The engine device, made through Microsoft's Direct3D 12 Agility SDK when a core is bundled beside
// the add-on. FSR 4's model needs Shader Model 6.6 with 8-bit integer dot products; the core Windows
// 10 ships answers 6.5, and FSR 4 then faults inside D3D12Core.dll on its first dispatch. OptiScaler
// calls the same fix FsrAgilitySDKUpgrade. `D3D12GetInterface` -> `ID3D12SDKConfiguration1::
// CreateDeviceFactory(version, path, ...)` -> `ID3D12DeviceFactory::CreateDevice` is the only route:
// a D3D12Core.dll next to the exe is inert unless the caller asks for it or the exe exports
// D3D12SDKVersion/D3D12SDKPath.
//
// Header-only and `static` on purpose: the add-on, the 32-bit helper and AeonSRPrebuild.exe all link
// this without a source file of its own, and no translation unit gets a GUID definition to duplicate.
#pragma once

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <string>

namespace aeon_sr {

// The D3D12SDKVersion the bundled D3D12Core.dll answers to. A core's own export of this name is what
// the loader compares against; 619 is Microsoft.Direct3D.D3D12 1.619.x.
inline constexpr UINT kAgilitySdkVersion = 619u;

namespace agility_detail {

// Defined here rather than taken from d3d12.h: including initguid.h in a header would give every
// translation unit its own definition of these.
inline const GUID kClsidSdkConfiguration =
	{ 0x7cda6aca, 0xa03e, 0x49c8, { 0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce } };
inline const GUID kIidSdkConfiguration1 =
	{ 0x8aaf9303, 0xad25, 0x48b9, { 0x9a, 0x57, 0xd9, 0xc3, 0x7e, 0x00, 0x9d, 0x9f } };
inline const GUID kIidDeviceFactory =
	{ 0x61f307d3, 0xd34e, 0x4e7c, { 0x83, 0x74, 0x3b, 0xa4, 0xde, 0x23, 0xcc, 0xcb } };

inline bool file_exists(const std::wstring &path)
{
	const DWORD a = GetFileAttributesW(path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool dir_exists(const std::wstring &path)
{
	const DWORD a = GetFileAttributesW(path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// The folder of the module this code is compiled into: for the add-on and AeonSRPrebuild.exe that is
// the add-on's own folder, which is where runtime\ sits.
inline std::wstring module_path_of(HMODULE module)
{
	wchar_t path[MAX_PATH * 2]{};
	const DWORD n = GetModuleFileNameW(module, path,
		static_cast<DWORD>(sizeof path / sizeof path[0]));
	if (n == 0 || n >= sizeof path / sizeof path[0])
		return std::wstring();
	std::wstring full(path, path + n);
	const auto slash = full.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

inline std::wstring own_module_directory()
{
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&own_module_directory), &self) ||
		self == nullptr)
		return std::wstring();
	return module_path_of(self);
}

inline std::wstring exe_directory()
{
	return module_path_of(nullptr);
}

// Ordered: the add-on's runtime folder (where packaging puts it), the add-on's folder, then the same
// two beside the game. First one holding D3D12Core.dll wins.
inline std::wstring find_bundled_core_dir(std::wstring *where = nullptr)
{
	std::wstring dirs[4];
	const std::wstring own = own_module_directory();
	const std::wstring exe = exe_directory();
	if (!own.empty()) {
		dirs[0] = own + L"\\runtime";
		dirs[1] = own;
	}
	if (!exe.empty()) {
		dirs[2] = exe + L"\\runtime";
		dirs[3] = exe;
	}
	for (const std::wstring &d : dirs) {
		if (d.empty() || !dir_exists(d))
			continue;
		if (file_exists(d + L"\\D3D12Core.dll")) {
			if (where != nullptr)
				*where = d;
			return d;
		}
	}
	return std::wstring();
}

// CreateDeviceFactory takes LPCSTR. A folder whose name is outside the ANSI code page needs its short
// (8.3) form or the loader reads the wrong path.
inline bool as_ansi_path(const std::wstring &dir, std::string *out)
{
	if (dir.empty() || out == nullptr)
		return false;
	const auto plain = [](const std::wstring &s) {
		for (const wchar_t c : s)
			if (c > 126 || c < 32)
				return false;
		return true;
	};
	if (!plain(dir)) {
		wchar_t short_path[MAX_PATH * 2]{};
		const DWORD n = GetShortPathNameW(dir.c_str(), short_path,
			static_cast<DWORD>(sizeof short_path / sizeof short_path[0]));
		if (n == 0 || n >= sizeof short_path / sizeof short_path[0] || !plain(short_path))
			return false;
		out->assign(short_path, short_path + n);
		return true;
	}
	out->assign(dir.begin(), dir.end());
	return true;
}

}  // namespace agility_detail

// True when a bundled core is present and Windows will hand out a device factory for this process.
// `say` names the folder and why not, for the log.
inline bool agility_core_ready(std::wstring *say = nullptr)
{
	std::wstring dir;
	if (agility_detail::find_bundled_core_dir(&dir).empty()) {
		if (say != nullptr)
			*say = L"no D3D12Core.dll beside the add-on";
		return false;
	}
	ID3D12SDKConfiguration *config = nullptr;
	HRESULT hr = D3D12GetInterface(agility_detail::kClsidSdkConfiguration, IID_PPV_ARGS(&config));
	if (FAILED(hr) || config == nullptr) {
		wchar_t buf[160]{};
		_snwprintf_s(buf, _TRUNCATE,
			L"this Windows cannot load a Direct3D 12 core other than its own (0x%08lX)",
			static_cast<unsigned long>(hr));
		if (say != nullptr)
			*say = buf;
		return false;
	}
	ID3D12SDKConfiguration1 *config1 = nullptr;
	hr = config->QueryInterface(agility_detail::kIidSdkConfiguration1, reinterpret_cast<void **>(&config1));
	config->Release();
	if (FAILED(hr) || config1 == nullptr) {
		if (say != nullptr)
			*say = L"this Windows has no Direct3D 12 device factory (it predates the Agility SDK)";
		return false;
	}
	config1->Release();
	if (say != nullptr)
		*say = dir;
	return true;
}

// Makes the device on the bundled core. Every failure leaves *device null and says why in `say`, and
// the caller falls back to the system core.
inline HRESULT create_device_on_agility_core(IDXGIAdapter *adapter, D3D_FEATURE_LEVEL level,
	REFIID riid, void **device, std::wstring *say = nullptr)
{
	if (device != nullptr)
		*device = nullptr;
	std::wstring dir;
	if (agility_detail::find_bundled_core_dir(&dir).empty()) {
		if (say != nullptr)
			*say = L"no D3D12Core.dll beside the add-on";
		return E_FAIL;
	}
	std::string path;
	if (!agility_detail::as_ansi_path(dir, &path)) {
		if (say != nullptr)
			*say = L"the folder with D3D12Core.dll has letters the core loader cannot take";
		return E_FAIL;
	}

	ID3D12SDKConfiguration *config = nullptr;
	HRESULT hr = D3D12GetInterface(agility_detail::kClsidSdkConfiguration, IID_PPV_ARGS(&config));
	if (FAILED(hr) || config == nullptr) {
		wchar_t buf[160]{};
		_snwprintf_s(buf, _TRUNCATE,
			L"this Windows cannot load a Direct3D 12 core other than its own (0x%08lX)",
			static_cast<unsigned long>(hr));
		if (say != nullptr)
			*say = buf;
		return hr;
	}
	ID3D12SDKConfiguration1 *config1 = nullptr;
	hr = config->QueryInterface(agility_detail::kIidSdkConfiguration1, reinterpret_cast<void **>(&config1));
	config->Release();
	if (FAILED(hr) || config1 == nullptr) {
		if (say != nullptr)
			*say = L"this Windows has no Direct3D 12 device factory (it predates the Agility SDK)";
		return FAILED(hr) ? hr : E_NOINTERFACE;
	}

	ID3D12DeviceFactory *factory = nullptr;
	hr = config1->CreateDeviceFactory(kAgilitySdkVersion, path.c_str(),
		agility_detail::kIidDeviceFactory, reinterpret_cast<void **>(&factory));
	config1->Release();
	if (FAILED(hr) || factory == nullptr) {
		wchar_t buf[400]{};
		_snwprintf_s(buf, _TRUNCATE,
			L"Windows would not load the Direct3D 12 core %u from %hs (0x%08lX)",
			kAgilitySdkVersion, path.c_str(), static_cast<unsigned long>(hr));
		if (say != nullptr)
			*say = buf;
		return hr;
	}

	hr = factory->CreateDevice(adapter, level, riid, device);
	factory->Release();
	if (FAILED(hr)) {
		if (say != nullptr) {
			wchar_t buf[400]{};
			_snwprintf_s(buf, _TRUNCATE,
				L"the Direct3D 12 core %u from %hs refused a device (0x%08lX)",
				kAgilitySdkVersion, path.c_str(), static_cast<unsigned long>(hr));
			*say = buf;
		}
		return hr;
	}
	if (say != nullptr)
		*say = dir;
	return S_OK;
}

}  // namespace aeon_sr

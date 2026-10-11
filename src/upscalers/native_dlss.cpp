#include "aeon_sr/upscalers/native_dlss.hpp"

#include "aeon_sr/ngx/ngx_common.hpp"

#include <Windows.h>
#include <psapi.h>

#pragma comment(lib, "version.lib")

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <mutex>

namespace aeon_sr {

namespace {

NativeDlss g_at_load;

std::wstring canonical(const std::wstring &path)
{
	std::wstring s = path;
	if (s.rfind(L"\\\\?\\", 0) == 0)
		s.erase(0, 4);
	for (wchar_t &c : s)
		c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
	return s;
}

std::wstring file_name(const std::wstring &canonical_path)
{
	const size_t slash = canonical_path.find_last_of(L'\\');
	return slash == std::wstring::npos ? canonical_path : canonical_path.substr(slash + 1);
}

std::wstring store_folder(const std::wstring &canonical_path)
{
	static const std::wstring kModels = L"\\ngx\\models\\";
	const size_t at = canonical_path.find(kModels);
	if (at == std::wstring::npos)
		return std::wstring();
	const size_t from = at + kModels.size();
	const size_t end = canonical_path.find(L'\\', from);
	return end == std::wstring::npos ? std::wstring() : canonical_path.substr(from, end - from);
}

bool is_one_of(const std::wstring &s, std::initializer_list<const wchar_t *> names)
{
	for (const wchar_t *n : names)
		if (s == n)
			return true;
	return false;
}

}

DlssModuleKind dlss_module_kind(const std::wstring &path)
{
	const std::wstring p = canonical(path);
	const std::wstring name = file_name(p);
	if (is_one_of(name, { L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll", L"sl.dlss_g.dll", L"sl.dlss_d.dll" }))
		return DlssModuleKind::Streamline;
	if (name == L"_nvngx.dll")
		return DlssModuleKind::NgxCore;
	if (name == L"nvngx_dlss.dll")
		return DlssModuleKind::Dlss;
	if (name == L"nvngx_dlssg.dll")
		return DlssModuleKind::FrameGeneration;
	if (name == L"nvngx_dlssd.dll")
		return DlssModuleKind::RayReconstruction;
	const std::wstring folder = store_folder(p);
	if (folder.empty())
		return DlssModuleKind::None;
	if (is_one_of(folder, { L"dlss", L"dlss_override" }))
		return DlssModuleKind::Dlss;
	if (is_one_of(folder, { L"dlssg", L"dlssg_override" }))
		return DlssModuleKind::FrameGeneration;
	if (is_one_of(folder, { L"dlssd", L"dlssd_override" }))
		return DlssModuleKind::RayReconstruction;
	if (folder.rfind(L"sl_", 0) == 0)
		return DlssModuleKind::Streamline;
	return DlssModuleKind::None;
}

bool dlss_module_from_driver_store(const std::wstring &path)
{
	return !store_folder(canonical(path)).empty();
}

bool same_module_file(const std::wstring &a, const std::wstring &b)
{
	if (a.empty() || b.empty())
		return false;
	if (canonical(a) == canonical(b))
		return true;
	const auto identity = [](const std::wstring &path, BY_HANDLE_FILE_INFORMATION *out) {
		const HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		const bool ok = GetFileInformationByHandle(h, out) != FALSE;
		CloseHandle(h);
		return ok;
	};
	BY_HANDLE_FILE_INFORMATION x{}, y{};
	return identity(a, &x) && identity(b, &y) && x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
		x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
}

NativeDlss classify_native_dlss(const std::vector<std::wstring> &module_paths, const std::wstring &own_runtime,
	bool own_ngx_started, const std::vector<std::wstring> &before_own)
{
	NativeDlss out;
	out.queried = true;
	std::vector<std::wstring> before;
	before.reserve(before_own.size());
	for (const std::wstring &b : before_own)
		before.push_back(canonical(b));
	std::vector<std::wstring> names;
	for (const std::wstring &path : module_paths) {
		const DlssModuleKind kind = dlss_module_kind(path);
		if (kind == DlssModuleKind::None)
			continue;
		bool counts = false;
		const auto own_load = [&]() {
			if (!own_ngx_started)
				return false;
			if (std::find(before.begin(), before.end(), canonical(path)) != before.end())
				return false;
			if (dlss_module_from_driver_store(path))
				return true;
			if (own_runtime.empty())
				return false;
			const std::wstring own = canonical(own_runtime);
			const size_t slash = own.find_last_of(L'\\');
			const std::wstring own_dir = slash == std::wstring::npos ? std::wstring() : own.substr(0, slash + 1);
			const std::wstring here = canonical(path);
			if (kind == DlssModuleKind::Dlss)
				return same_module_file(path, own_runtime);
			const bool beside = here.rfind(own_dir, 0) == 0 && here.find(L'\\', own_dir.size()) == std::wstring::npos;
			return !own_dir.empty() && (beside || same_module_file(path, own_dir + file_name(here)));
		};
		switch (kind) {
		case DlssModuleKind::Streamline:
			out.streamline = true;
			counts = true;
			break;
		case DlssModuleKind::NgxCore:
			out.ngx_core = true;
			break;
		case DlssModuleKind::Dlss:
			out.dlss = true;
			counts = !own_load();
			break;
		case DlssModuleKind::FrameGeneration:
			out.frame_generation = true;
			counts = !own_load();
			break;
		case DlssModuleKind::RayReconstruction:
			out.ray_reconstruction = true;
			counts = !own_load();
			break;
		default:
			break;
		}
		const size_t slash = path.find_last_of(L"\\/");
		const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
		const std::wstring folder = store_folder(canonical(path));
		const std::wstring label = folder.empty() ? name : name + L" (the driver's " + folder + L")";
		if (std::find(names.begin(), names.end(), label) == names.end()) {
			names.push_back(label);
			if (!out.modules.empty())
				out.modules += L", ";
			out.modules += label;
		}
		if (counts && !out.native) {
			out.native = true;
			out.dlss_path = path;
			out.source = label;
		}
	}
	return out;
}

NativeDlss scan_native_dlss(const std::wstring &own_runtime)
{
	static std::mutex m;
	static std::vector<HMODULE> last_handles;
	static bool last_own = false;
	static std::wstring last_runtime;
	static NativeDlss last;
	std::lock_guard<std::mutex> lock(m);
	std::vector<HMODULE> handles(512);
	DWORD need = 0;
	bool listed = false;
	for (int tries = 0; tries < 4; ++tries) {
		listed = K32EnumProcessModules(GetCurrentProcess(), handles.data(),
			static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &need) != FALSE;
		if (!listed || need <= handles.size() * sizeof(HMODULE))
			break;
		handles.resize(need / sizeof(HMODULE) + 64);
		listed = false;
	}
	if (listed)
		handles.resize(need / sizeof(HMODULE));
	const bool own = own_ngx_started();
	if (listed && last.queried && handles == last_handles && own == last_own && own_runtime == last_runtime)
		return last;
	last = classify_native_dlss(loaded_module_paths(), own_runtime, own, modules_before_own_ngx());
	last_handles = listed ? std::move(handles) : std::vector<HMODULE>();
	last_own = own;
	last_runtime = own_runtime;
	return last;
}

bool ngx_layer_candidate(const std::wstring &path, bool exports_ngx)
{
	if (!exports_ngx || path.empty())
		return false;
	const std::wstring p = canonical(path);
	const std::wstring name = file_name(p);
	if (name.rfind(L"nvngx_", 0) == 0 || name == L"_nvngx.dll" || dlss_module_kind(path) != DlssModuleKind::None)
		return false;
	if (dlss_module_from_driver_store(path))
		return false;
	for (const wchar_t *dir : { L"\\system32\\", L"\\syswow64\\" })
		if (p.find(dir) != std::wstring::npos)
			return false;
	return true;
}

namespace {

std::wstring version_string(const std::wstring &path, const wchar_t *field)
{
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
	if (size == 0)
		return std::wstring();
	std::vector<unsigned char> data(size);
	if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
		return std::wstring();
	struct LangCodepage {
		WORD language, codepage;
	} *pairs = nullptr;
	UINT bytes = 0;
	if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&pairs), &bytes) ||
		bytes < sizeof(LangCodepage))
		return std::wstring();
	wchar_t key[96];
	std::swprintf(key, 96, L"\\StringFileInfo\\%04x%04x\\%ls", pairs[0].language, pairs[0].codepage, field);
	wchar_t *value = nullptr;
	UINT len = 0;
	if (VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&value), &len) && value != nullptr && len > 1)
		return std::wstring(value);
	return std::wstring();
}

std::wstring lower(std::wstring s)
{
	for (wchar_t &c : s)
		c = static_cast<wchar_t>(std::towlower(c));
	return s;
}

}

std::vector<NgxLayer> scan_ngx_layers(void *self)
{
	static std::mutex m;
	static std::vector<HMODULE> last_handles;
	static std::vector<NgxLayer> last;
	static void *last_self = nullptr;
	std::lock_guard<std::mutex> lock(m);
	std::vector<NgxLayer> out;
	std::vector<HMODULE> mods(512);
	DWORD need = 0;
	for (int tries = 0; tries < 4; ++tries) {
		if (!K32EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)),
				&need))
			return out;
		if (need <= mods.size() * sizeof(HMODULE))
			break;
		mods.resize(need / sizeof(HMODULE) + 64);
	}
	const size_t n = std::min<size_t>(need / sizeof(HMODULE), mods.size());
	mods.resize(n);
	if (mods == last_handles && self == last_self)
		return last;
	wchar_t buf[MAX_PATH * 2];
	for (size_t i = 0; i < n; ++i) {
		if (mods[i] == static_cast<HMODULE>(self))
			continue;
		const bool exports_ngx = GetProcAddress(mods[i], "NVSDK_NGX_D3D12_Init") != nullptr ||
			GetProcAddress(mods[i], "NVSDK_NGX_D3D11_Init") != nullptr ||
			GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_Init") != nullptr;
		if (!exports_ngx)
			continue;
		const DWORD len = GetModuleFileNameW(mods[i], buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
		if (len == 0 || len >= sizeof(buf) / sizeof(buf[0]))
			continue;
		const std::wstring path(buf, len);
		if (!ngx_layer_candidate(path, true))
			continue;
		if (lower(version_string(path, L"CompanyName")).find(L"nvidia") != std::wstring::npos)
			continue;
		NgxLayer layer;
		layer.path = path;
		std::wstring product = version_string(path, L"ProductName");
		if (product.empty())
			product = version_string(path, L"FileDescription");
		layer.optiscaler = lower(product).find(L"optiscaler") != std::wstring::npos;
		layer.name = layer.optiscaler ? L"OptiScaler" : product;
		out.push_back(std::move(layer));
	}
	last_handles = std::move(mods);
	last = out;
	last_self = self;
	return out;
}

std::wstring ngx_layer_label(const NgxLayer &layer)
{
	const size_t slash = layer.path.find_last_of(L"\\/");
	const std::wstring file = slash == std::wstring::npos ? layer.path : layer.path.substr(slash + 1);
	if (layer.optiscaler)
		return L"OptiScaler (" + file + L")";
	return L"a DLSS layer (" + file + (layer.name.empty() ? L")" : L", " + layer.name + L")");
}

void snapshot_native_dlss()
{
	g_at_load = classify_native_dlss(loaded_module_paths(), std::wstring(), false);
}

const NativeDlss &native_dlss_at_load() noexcept
{
	return g_at_load;
}

}

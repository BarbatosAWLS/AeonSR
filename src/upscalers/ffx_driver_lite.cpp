#include "aeon_sr/upscalers/ffx_driver_lite.hpp"
#include "aeon_sr/upscalers/runtime_load_check.hpp"

#include <d3d12.h>

#include <ffx_api.h>
#include <ffx_upscale.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>

#pragma comment(lib, "version.lib")

namespace aeon_sr {

namespace {

struct ProviderInterface {
	uint64_t version_id;
	const char *version_name;
	bool (*can_provide)(uint64_t desc_type);
	PfnFfxCreateContext create_context;
	PfnFfxDestroyContext destroy_context;
	PfnFfxConfigure configure;
	PfnFfxQuery query;
	PfnFfxDispatch dispatch;
};
struct ExternalProviderData {
	uint32_t struct_version;
	uint64_t desc_type;
	ProviderInterface provider;
};
static_assert(sizeof(ExternalProviderData) == 0x50, "the driver reads exactly 0x50 bytes at struct version 2");

struct ProviderOption {
	uint32_t type;
	uint32_t effect;
	bool enable;
	ProviderOption *next;
};
static_assert(sizeof(ProviderOption) == 0x18, "the driver walks 0x18-byte options");

using UpdateProviderFn = HRESULT(STDMETHODCALLTYPE *)(void *data, uint32_t size);
using UpdateProviderExFn = HRESULT(STDMETHODCALLTYPE *)(void *data, uint32_t size, ProviderOption *options);
using GetModuleHandleWFn = HMODULE(WINAPI *)(LPCWSTR);
using IsSupportedFn = bool (*)(void *provider, void *device);

constexpr const char *kModelClasses[] = {
	".?AVffxProvider_FSR4@@",
	".?AVffxProvider_FSR4_Int8@@",
	".?AVffxProvider_FSR4_Int8_Navi2@@",
	".?AVffxProvider_FSR4Lite_Int8@@",
};
constexpr size_t kModels = sizeof(kModelClasses) / sizeof(kModelClasses[0]);
static_assert(kModels == static_cast<size_t>(FfxDriverModel::Count), "one class per model");
constexpr size_t kIsSupportedSlot = 2;

struct KnownBuild {
	uint16_t major, minor, build;
	const char *fsr;
};
constexpr KnownBuild kKnownBuilds[] = {
	{ 2, 4, 0, "4.1.2" },
};

std::mutex g_mutex;
HMODULE g_module = nullptr;
std::wstring g_path;
FfxApi g_api{};
FfxDriverLite g_info{};
std::wstring g_error;
bool g_touched = false;

std::atomic<HMODULE> g_self{ nullptr };
std::atomic<GetModuleHandleWFn> g_real_get_module{ nullptr };
std::atomic<uint32_t> g_active{ static_cast<uint32_t>(FfxDriverModel::Count) };
PfnFfxQuery g_query = nullptr;
uint64_t g_version_id = 0;

HMODULE WINAPI driver_get_module_handle(LPCWSTR name)
{
	if (ffx_sdk_module_name(name)) {
		if (const HMODULE self = g_self.load(std::memory_order_acquire))
			return self;
	}
	const GetModuleHandleWFn real = g_real_get_module.load(std::memory_order_acquire);
	return real != nullptr ? real(name) : GetModuleHandleW(name);
}

template <size_t I>
bool model_supported(void *provider, void *device)
{
	(void)provider;
	const bool yes = ffx_driver_model_allowed(static_cast<FfxDriverModel>(I));
	if (yes && device != nullptr)
		g_active.store(static_cast<uint32_t>(I), std::memory_order_release);
	return yes;
}
constexpr IsSupportedFn kModelHooks[] = { model_supported<0>, model_supported<1>, model_supported<2>, model_supported<3> };
static_assert(sizeof(kModelHooks) / sizeof(kModelHooks[0]) == kModels, "one hook per model");

ffxReturnCode_t driver_query(ffxContext *context, ffxQueryDescHeader *desc)
{
	if (desc == nullptr)
		return FFX_API_RETURN_ERROR_PARAMETER;
	if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION && context != nullptr && *context != nullptr) {
		auto *q = reinterpret_cast<ffxQueryGetProviderVersion *>(desc);
		q->versionId = g_version_id;
		q->versionName = g_info.version_name.c_str();
		return FFX_API_RETURN_OK;
	}
	if (context == nullptr || *context == nullptr) {
		if (desc->type != FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE)
			return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
		auto *q = reinterpret_cast<ffxQueryDescUpscaleGetRenderResolutionFromQualityMode *>(desc);
		uint32_t w = 0, h = 0;
		if (!fsr_render_size_for_quality(q->displayWidth, q->displayHeight, q->qualityMode, &w, &h))
			return FFX_API_RETURN_ERROR_PARAMETER;
		if (q->pOutRenderWidth != nullptr)
			*q->pOutRenderWidth = w;
		if (q->pOutRenderHeight != nullptr)
			*q->pOutRenderHeight = h;
		return FFX_API_RETURN_OK;
	}
	return g_query(context, desc);
}

struct Image {
	BYTE *base = nullptr;
	IMAGE_NT_HEADERS *nt = nullptr;
};

bool image_of(HMODULE module, Image *out) noexcept
{
	if (module == nullptr)
		return false;
	auto *const base = reinterpret_cast<BYTE *>(module);
	const auto *const dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	auto *const nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;
	out->base = base;
	out->nt = nt;
	return true;
}

template <typename Accept>
BYTE *find_in_image(const Image &img, const void *needle, size_t n, size_t align, Accept accept) noexcept
{
	const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(img.nt);
	const BYTE first = *static_cast<const BYTE *>(needle);
	for (WORD s = 0; s < img.nt->FileHeader.NumberOfSections; ++s) {
		BYTE *p = img.base + sec[s].VirtualAddress;
		BYTE *const hi = p + sec[s].Misc.VirtualSize;
		while (p + n <= hi) {
			BYTE *const c = static_cast<BYTE *>(std::memchr(p, first, static_cast<size_t>(hi - p - n + 1)));
			if (c == nullptr)
				break;
			if (static_cast<size_t>(c - img.base) % align == 0 && std::memcmp(c, needle, n) == 0 && accept(c))
				return c;
			p = c + 1;
		}
	}
	return nullptr;
}

bool in_code(const Image &img, const void *p) noexcept
{
	const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(img.nt);
	const auto *const b = static_cast<const BYTE *>(p);
	for (WORD s = 0; s < img.nt->FileHeader.NumberOfSections; ++s) {
		if ((sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
			continue;
		const BYTE *const lo = img.base + sec[s].VirtualAddress;
		if (b >= lo && b < lo + sec[s].Misc.VirtualSize)
			return true;
	}
	return false;
}

bool same_path(const std::wstring &a, const std::wstring &b)
{
	return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool write_pointer(void **slot, void *value) noexcept
{
	DWORD old = 0;
	if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
		return false;
	*slot = value;
	VirtualProtect(slot, sizeof(void *), old, &old);
	return true;
}

bool file_version(const std::wstring &path, uint16_t v[4], std::wstring *product)
{
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
	if (size == 0)
		return false;
	std::vector<BYTE> data(size);
	if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
		return false;
	VS_FIXEDFILEINFO *ffi = nullptr;
	UINT len = 0;
	if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&ffi), &len) || ffi == nullptr ||
		len < sizeof(VS_FIXEDFILEINFO) || ffi->dwSignature != 0xFEEF04BDu)
		return false;
	v[0] = HIWORD(ffi->dwFileVersionMS);
	v[1] = LOWORD(ffi->dwFileVersionMS);
	v[2] = HIWORD(ffi->dwFileVersionLS);
	v[3] = LOWORD(ffi->dwFileVersionLS);
	if (product != nullptr) {
		product->clear();
		struct Lang { WORD lang, cp; } *langs = nullptr;
		if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&langs), &len) &&
			langs != nullptr && len >= sizeof(Lang)) {
			wchar_t key[64]{};
			_snwprintf_s(key, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\ProductName", langs[0].lang, langs[0].cp);
			wchar_t *name = nullptr;
			if (VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&name), &len) && name != nullptr)
				product->assign(name);
		}
	}
	return true;
}

std::string known_build(const std::wstring &path, std::wstring *why)
{
	uint16_t v[4]{};
	std::wstring product;
	if (!file_version(path, v, &product)) {
		if (why != nullptr)
			*why = L"amdxcffx64.dll has no version information: not AMD's driver FidelityFX library";
		return std::string();
	}
	const std::string fsr = fsr_version_of_driver(v[0], v[1], v[2]);
	if (fsr.empty() && why != nullptr) {
		wchar_t buf[200]{};
		_snwprintf_s(buf, _TRUNCATE,
			L"amdxcffx64.dll is FidelityFX %u.%u.%u.%u; FSR 4 Lite knows the layout of 2.4.0 (FSR 4.1.2) only",
			v[0], v[1], v[2], v[3]);
		*why = buf;
	}
	return fsr;
}

void load_locked(const std::wstring &path)
{
	g_error.clear();
	if (known_build(path, &g_error).empty())
		return;
	HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (module == nullptr) {
		wchar_t buf[96]{};
		_snwprintf_s(buf, _TRUNCATE, L"LoadLibrary of %s failed (0x%08lX)", kFfxDriverDll, GetLastError());
		g_error = buf;
		return;
	}
	std::wstring got;
	if (!loaded_module_is(module, path, &got)) {
		g_error = runtime_load_redirect_error(path, got);
		return;
	}
	const auto update = reinterpret_cast<UpdateProviderFn>(GetProcAddress(module, "UpdateFfxApiProvider"));
	const auto update_ex = reinterpret_cast<UpdateProviderExFn>(GetProcAddress(module, "UpdateFfxApiProviderEx"));
	Image img;
	void **vtables[kModels]{};
	bool laid_out = update != nullptr && image_of(module, &img);
	for (size_t i = 0; laid_out && i < kModels; ++i) {
		vtables[i] = msvc_rtti_vtable(module, kModelClasses[i]);
		laid_out = vtables[i] != nullptr && in_code(img, vtables[i][kIsSupportedSlot]);
	}
	if (!laid_out) {
		FreeLibrary(module);
		g_error = L"amdxcffx64.dll does not have the four FSR 4 models where FidelityFX 2.4.0 keeps them";
		return;
	}

	HMODULE pinned = nullptr, self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<LPCWSTR>(module), &pinned) ||
		!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<LPCWSTR>(&driver_get_module_handle), &self)) {
		FreeLibrary(module);
		g_error = L"could not keep amdxcffx64.dll and the add-on loaded";
		return;
	}
	g_touched = true;
	g_self.store(module, std::memory_order_release);
	void *real_get_module = nullptr;
	if (!patch_module_import(module, "KERNEL32.dll", "GetModuleHandleW",
			reinterpret_cast<void *>(&driver_get_module_handle), &real_get_module)) {
		g_error = L"amdxcffx64.dll does not import GetModuleHandleW where this add-on expects it";
		return;
	}
	g_real_get_module.store(reinterpret_cast<GetModuleHandleWFn>(real_get_module), std::memory_order_release);
	int hooked = 0;
	for (size_t i = 0; i < kModels; ++i) {
		if (write_pointer(&vtables[i][kIsSupportedSlot], reinterpret_cast<void *>(kModelHooks[i])))
			++hooked;
	}
	g_info.model_hooks = hooked;
	if (hooked != static_cast<int>(kModels)) {
		g_error = L"could not answer amdxcffx64.dll's model checks";
		return;
	}

	ExternalProviderData data{};
	data.struct_version = 2;
	data.desc_type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	ProviderOption upscale{ 0u, 0u, true, nullptr };
	const HRESULT hr = update_ex != nullptr
		? update_ex(&data, static_cast<uint32_t>(sizeof(data)), &upscale)
		: update(&data, static_cast<uint32_t>(sizeof(data)));
	const ProviderInterface &p = data.provider;
	if (FAILED(hr) || p.create_context == nullptr || p.destroy_context == nullptr || p.configure == nullptr ||
		p.query == nullptr || p.dispatch == nullptr) {
		wchar_t buf[160]{};
		_snwprintf_s(buf, _TRUNCATE, L"amdxcffx64.dll handed out no upscaler (UpdateFfxApiProvider 0x%08lX)",
			static_cast<unsigned long>(hr));
		g_error = buf;
		return;
	}
	g_info.version_name = p.version_name != nullptr ? p.version_name : "";
	while (!g_info.version_name.empty() &&
		(g_info.version_name.back() == '*' || g_info.version_name.back() == ' '))
		g_info.version_name.pop_back();
	g_version_id = p.version_id;
	g_query = p.query;
	g_api.module = module;
	g_api.CreateContext = p.create_context;
	g_api.DestroyContext = p.destroy_context;
	g_api.Configure = p.configure;
	g_api.Query = &driver_query;
	g_api.Dispatch = p.dispatch;
	g_module = module;
	g_path = path;
}

}

bool ffx_sdk_module_name(const wchar_t *name) noexcept
{
	static constexpr wchar_t kPrefix[] = L"amd_fidelityfx_";
	return name != nullptr && _wcsnicmp(name, kPrefix, sizeof(kPrefix) / sizeof(kPrefix[0]) - 1) == 0;
}

bool ffx_driver_model_allowed(FfxDriverModel model) noexcept
{
	return model == FfxDriverModel::Int8Lite;
}

const char *ffx_driver_model_name(FfxDriverModel model) noexcept
{
	switch (model) {
	case FfxDriverModel::Fp8: return "FP8";
	case FfxDriverModel::Int8: return "INT8";
	case FfxDriverModel::Int8Navi2: return "INT8 RDNA 2";
	case FfxDriverModel::Int8Lite: return "INT8 Lite";
	default: return "none";
	}
}

FfxDriverModel ffx_driver_active_model() noexcept
{
	return static_cast<FfxDriverModel>(g_active.load(std::memory_order_acquire));
}

std::string fsr_version_of_driver(uint16_t major, uint16_t minor, uint16_t build) noexcept
{
	for (const KnownBuild &k : kKnownBuilds) {
		if (k.major == major && k.minor == minor && k.build == build)
			return k.fsr;
	}
	return std::string();
}

std::string ffx_driver_lite_entry(const std::wstring &path)
{
	if (path.empty())
		return std::string();
	const std::string fsr = known_build(path, nullptr);
	return fsr.empty() ? std::string() : fsr + std::string(kFsrLiteSuffix);
}

bool is_ffx_driver_path(const std::wstring &path) noexcept
{
	const size_t slash = path.find_last_of(L"\\/");
	const wchar_t *const file = path.c_str() + (slash == std::wstring::npos ? 0 : slash + 1);
	return _wcsicmp(file, kFfxDriverDll) == 0;
}

bool fsr_lite_device_supported(ID3D12Device *device, std::wstring *why)
{
	if (device == nullptr) {
		if (why != nullptr)
			*why = L"no Direct3D 12 device";
		return false;
	}
	D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_6 };
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
		sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
		if (why != nullptr)
			*why = L"this GPU or driver has no Shader Model 6.6, which FSR 4 Lite needs";
		return false;
	}
	return true;
}

bool fsr_render_size_for_quality(uint32_t display_w, uint32_t display_h, uint32_t quality,
	uint32_t *w, uint32_t *h) noexcept
{
	float ratio = 0.0f;
	switch (quality) {
	case FFX_UPSCALE_QUALITY_MODE_NATIVEAA: ratio = 1.0f; break;
	case FFX_UPSCALE_QUALITY_MODE_QUALITY: ratio = 1.5f; break;
	case FFX_UPSCALE_QUALITY_MODE_BALANCED: ratio = 1.7f; break;
	case FFX_UPSCALE_QUALITY_MODE_PERFORMANCE: ratio = 2.0f; break;
	case FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE: ratio = 3.0f; break;
	default: return false;
	}
	const auto side = [ratio](uint32_t d) { return static_cast<uint32_t>(static_cast<float>(d) / ratio); };
	*w = side(display_w);
	*h = side(display_h);
	return true;
}

void **msvc_rtti_vtable(HMODULE module, const char *mangled) noexcept
{
	Image img;
	if (mangled == nullptr || *mangled == '\0' || !image_of(module, &img))
		return nullptr;
	BYTE *const name = find_in_image(img, mangled, std::strlen(mangled) + 1, 1, [](BYTE *) { return true; });
	if (name == nullptr || name - img.base < 0x10)
		return nullptr;
	const DWORD td_rva = static_cast<DWORD>(name - 0x10 - img.base);
	BYTE *const td_field = find_in_image(img, &td_rva, sizeof(td_rva), 4, [&img](BYTE *hit) {
		if (hit - img.base < 12)
			return false;
		const DWORD *const col = reinterpret_cast<const DWORD *>(hit - 12);
		return col[0] == 1u && col[1] == 0u &&
			col[5] == static_cast<DWORD>(reinterpret_cast<const BYTE *>(col) - img.base);
	});
	if (td_field == nullptr)
		return nullptr;
	const ULONG_PTR col_va = reinterpret_cast<ULONG_PTR>(td_field - 12);
	BYTE *const slot = find_in_image(img, &col_va, sizeof(col_va), sizeof(col_va), [](BYTE *) { return true; });
	return slot != nullptr ? reinterpret_cast<void **>(slot + sizeof(void *)) : nullptr;
}

bool patch_module_import(HMODULE module, const char *dll, const char *function, void *hook,
	void **original) noexcept
{
	Image img;
	if (dll == nullptr || function == nullptr || !image_of(module, &img))
		return false;
	const IMAGE_DATA_DIRECTORY dir = img.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (dir.VirtualAddress == 0)
		return false;
	for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(img.base + dir.VirtualAddress); imp->Name != 0; ++imp) {
		if (_stricmp(reinterpret_cast<const char *>(img.base + imp->Name), dll) != 0 || imp->OriginalFirstThunk == 0)
			continue;
		auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(img.base + imp->OriginalFirstThunk);
		auto *iat = reinterpret_cast<IMAGE_THUNK_DATA *>(img.base + imp->FirstThunk);
		for (; names->u1.AddressOfData != 0; ++names, ++iat) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			const auto *const by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(img.base + names->u1.AddressOfData);
			if (std::strcmp(reinterpret_cast<const char *>(by_name->Name), function) != 0)
				continue;
			void *const was = reinterpret_cast<void *>(iat->u1.Function);
			if (!write_pointer(reinterpret_cast<void **>(&iat->u1.Function), hook))
				return false;
			if (original != nullptr)
				*original = was;
			return true;
		}
	}
	return false;
}

bool ffx_load_driver_lite(FfxApi &api, const std::wstring &path, FfxDriverLite *info, std::wstring &error)
{
	ffx_unload(api);
	error.clear();
	if (path.empty()) {
		error = L"no amdxcffx64.dll was found";
		return false;
	}
	const std::lock_guard<std::mutex> lock(g_mutex);
	if (g_module != nullptr) {
		if (!same_path(g_path, path))
			error = L"another amdxcffx64.dll is already in use in this process: " + g_path;
	} else if (!g_touched) {
		load_locked(path);
		if (g_module == nullptr && g_error.empty())
			g_error = L"amdxcffx64.dll could not be loaded";
		error = g_error;
	} else {
		error = g_error;
	}
	if (!error.empty())
		return false;
	api = g_api;
	if (info != nullptr)
		*info = g_info;
	return true;
}

}

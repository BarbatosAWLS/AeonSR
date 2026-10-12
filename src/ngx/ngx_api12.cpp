#include "aeon_sr/ngx/ngx_api12.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/ngx/ngx_override.hpp"

#include <nvsdk_ngx.h>

#include <cstdio>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <vector>

#pragma comment(lib, "version.lib")

namespace aeon_sr {
namespace detail {

NVSDK_NGX_Result ngx12_route_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Feature feature,
	NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out);
NVSDK_NGX_Result ngx12_route_evaluate(ID3D12GraphicsCommandList *cmd, const NVSDK_NGX_Handle *handle,
	const NVSDK_NGX_Parameter *params, PFN_NVSDK_NGX_ProgressCallback_C callback);

}
}

#ifdef NVSDK_NGX_HELPERS_H
#error "nvsdk_ngx_helpers.h reached this file before the renames below: its helpers would call the static library"
#endif
#define NVSDK_NGX_D3D12_CreateFeature(...) ::aeon_sr::detail::ngx12_route_create(__VA_ARGS__)
#define NVSDK_NGX_D3D12_EvaluateFeature_C(...) ::aeon_sr::detail::ngx12_route_evaluate(__VA_ARGS__)
#include <nvsdk_ngx_helpers.h>
#undef NVSDK_NGX_D3D12_CreateFeature
#undef NVSDK_NGX_D3D12_EvaluateFeature_C

namespace aeon_sr {
namespace {

using PfnInitExt = NVSDK_NGX_Result(NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D12Device *,
	NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
using PfnInitProject = NVSDK_NGX_Result(NVSDK_CONV *)(const char *, NVSDK_NGX_EngineType, const char *,
	const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
using PfnShutdown = NVSDK_NGX_Result(NVSDK_CONV *)();
using PfnShutdown1 = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12Device *);
using PfnParams = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter **);
using PfnDestroy = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter *);
using PfnCreate = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
	NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using PfnEvaluate = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
	const NVSDK_NGX_Parameter *, void *);
using PfnRelease = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);

template <class F>
F as(void *p) noexcept
{
	return reinterpret_cast<F>(p);
}

thread_local const Ngx12Api *t_api = nullptr;

struct ApiScope {
	explicit ApiScope(const Ngx12Api *api) noexcept : prev(t_api) { t_api = api; }
	~ApiScope() { t_api = prev; }
	const Ngx12Api *prev;
};

std::wstring lower(std::wstring s)
{
	for (wchar_t &c : s)
		c = static_cast<wchar_t>(std::towlower(c));
	return s;
}

std::wstring parent_dir(const std::wstring &path)
{
	const size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring module_path(HMODULE m)
{
	wchar_t buf[MAX_PATH * 2]{};
	const DWORD n = GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf)));
	return n == 0 || n >= std::size(buf) ? std::wstring() : std::wstring(buf, n);
}

void set_d4r_states()
{
	char value[16]{};
	for (const char *name : { "D4R_SHIM_INPUT_STATE", "D4R_SHIM_DEPTH_STATE" }) {
		if (GetEnvironmentVariableA(name, value, sizeof(value)) == 0)
			SetEnvironmentVariableA(name, "0xC0");
	}
}

std::wstring product_of(const std::wstring &path)
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
	for (const wchar_t *field : { L"ProductName", L"FileDescription" }) {
		wchar_t key[96];
		std::swprintf(key, 96, L"\\StringFileInfo\\%04x%04x\\%ls", pairs[0].language, pairs[0].codepage, field);
		wchar_t *value = nullptr;
		UINT len = 0;
		if (VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&value), &len) && value != nullptr && len > 1)
			return std::wstring(value);
	}
	return std::wstring();
}

}

Ngx12Api ngx12_load_module(const std::wstring &path)
{
	Ngx12Api api;
	api.path = path;
	if (lower(product_of(path)).find(L"optiscaler") != std::wstring::npos) {
		api.error = L"it is OptiScaler, not an NGX runtime. Put d4r's own nvngx.dll there";
		return api;
	}
	HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (m == nullptr) {
		wchar_t buf[64]{};
		_snwprintf_s(buf, _TRUNCATE, L" (error %lu)", GetLastError());
		api.error = L"Windows would not load it" + std::wstring(buf);
		return api;
	}
	const std::wstring got = module_path(m);
	if (!got.empty() && !ngx_same_file(got, path)) {
		api.error = L"another program answered its load with " + got +
			L" (OptiScaler does this; remove it, or set HookOriginalNvngxOnly=true in OptiScaler.ini)";
		return api;
	}
	api.module = m;
	api.fn_init_ext = reinterpret_cast<void *>(GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext"));
	api.fn_init_project = reinterpret_cast<void *>(GetProcAddress(m, "NVSDK_NGX_D3D12_Init_ProjectID"));
	api.fn_shutdown = reinterpret_cast<void *>(GetProcAddress(m, "NVSDK_NGX_D3D12_Shutdown"));
	api.fn_shutdown1 = reinterpret_cast<void *>(GetProcAddress(m, "NVSDK_NGX_D3D12_Shutdown1"));
	api.fn_capability = reinterpret_cast<void *>(GetProcAddress(m, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
	api.fn_allocate = reinterpret_cast<void *>(GetProcAddress(m, "NVSDK_NGX_D3D12_AllocateParameters"));
	api.fn_destroy = reinterpret_cast<void *>(GetProcAddress(m, "NVSDK_NGX_D3D12_DestroyParameters"));
	api.fn_create = reinterpret_cast<void *>(GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature"));
	api.fn_evaluate = reinterpret_cast<void *>(GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature"));
	api.fn_release = reinterpret_cast<void *>(GetProcAddress(m, "NVSDK_NGX_D3D12_ReleaseFeature"));
	std::wstring missing;
	const struct { void *fn; const wchar_t *name; } required[] = {
		{ api.fn_init_ext, L"Init_Ext" }, { api.fn_shutdown != nullptr ? api.fn_shutdown : api.fn_shutdown1, L"Shutdown" },
		{ api.fn_capability, L"GetCapabilityParameters" }, { api.fn_allocate, L"AllocateParameters" },
		{ api.fn_destroy, L"DestroyParameters" }, { api.fn_create, L"CreateFeature" },
		{ api.fn_evaluate, L"EvaluateFeature" }, { api.fn_release, L"ReleaseFeature" },
	};
	for (const auto &r : required) {
		if (r.fn == nullptr)
			missing += (missing.empty() ? L"NVSDK_NGX_D3D12_" : L", NVSDK_NGX_D3D12_") + std::wstring(r.name);
	}
	if (!missing.empty())
		api.error = L"it is not an NGX runtime: it does not export " + missing;
	return api;
}

const Ngx12Api &ngx12_static_api()
{
	static const Ngx12Api api;
	return api;
}

const Ngx12Api &ngx12_api(const std::wstring &addon_dir)
{
	static std::once_flag once;
	static Ngx12Api loaded;
	static const Ngx12Api *chosen = nullptr;
	std::call_once(once, [&addon_dir] {
		chosen = &ngx12_static_api();
		const std::wstring &path = ngx_override_path(addon_dir);
		if (path.empty())
			return;
		chosen = &loaded;
		if (GetModuleHandleW(path.c_str()) != nullptr && !own_ngx_started()) {
			loaded.path = path;
			loaded.error = L"another program in this game loaded it first (OptiScaler?); sharing its one "
				L"instance would end that program's DLSS whenever Aeon SR's stops";
		} else {
			note_own_ngx_start();
			set_d4r_states();
			loaded = ngx12_load_module(path);
		}
		diag_state("ngx-runtime", loaded.usable() ? DiagLevel::Info : DiagLevel::Error, "dlss", loaded.usable()
			? L"DLSS runs through " + path + L" instead of NVIDIA's driver"
			: L"DLSS was to run through " + path + L", but " + loaded.error);
	});
	return *chosen;
}

std::wstring ngx12_feature_dir(const Ngx12Api &api, const std::wstring &fallback)
{
	if (api.external()) {
		const std::wstring own = parent_dir(api.path);
		if (!own.empty() && file_exists_w(ngx_dll_path(own)))
			return own;
	}
	return fallback;
}

NVSDK_NGX_Result Ngx12Api::init(unsigned long long app_id, const wchar_t *data_dir, ID3D12Device *device,
	const NVSDK_NGX_FeatureCommonInfo *info) const
{
	if (!external())
		return NVSDK_NGX_D3D12_Init(app_id, data_dir, device, info, NVSDK_NGX_Version_API);
	if (!usable())
		return NVSDK_NGX_Result_FAIL_PlatformError;
	return as<PfnInitExt>(fn_init_ext)(app_id, data_dir, device, NVSDK_NGX_Version_API, info);
}

NVSDK_NGX_Result Ngx12Api::init_project(const char *project_id, NVSDK_NGX_EngineType engine,
	const char *engine_version, const wchar_t *data_dir, ID3D12Device *device,
	const NVSDK_NGX_FeatureCommonInfo *info) const
{
	if (!external())
		return NVSDK_NGX_D3D12_Init_with_ProjectID(project_id, engine, engine_version, data_dir, device, info,
			NVSDK_NGX_Version_API);
	if (!usable() || fn_init_project == nullptr)
		return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
	return as<PfnInitProject>(fn_init_project)(project_id, engine, engine_version, data_dir, device,
		NVSDK_NGX_Version_API, info);
}

NVSDK_NGX_Result Ngx12Api::shutdown(ID3D12Device *device) const
{
	if (!external())
		return NVSDK_NGX_D3D12_Shutdown1(device);
	if (!usable())
		return NVSDK_NGX_Result_FAIL_NotInitialized;
	if (fn_shutdown != nullptr)
		return as<PfnShutdown>(fn_shutdown)();
	return as<PfnShutdown1>(fn_shutdown1)(device);
}

NVSDK_NGX_Result Ngx12Api::capability_parameters(NVSDK_NGX_Parameter **out) const
{
	if (!external())
		return NVSDK_NGX_D3D12_GetCapabilityParameters(out);
	return usable() ? as<PfnParams>(fn_capability)(out) : NVSDK_NGX_Result_FAIL_NotInitialized;
}

NVSDK_NGX_Result Ngx12Api::allocate_parameters(NVSDK_NGX_Parameter **out) const
{
	if (!external())
		return NVSDK_NGX_D3D12_AllocateParameters(out);
	return usable() ? as<PfnParams>(fn_allocate)(out) : NVSDK_NGX_Result_FAIL_NotInitialized;
}

NVSDK_NGX_Result Ngx12Api::destroy_parameters(NVSDK_NGX_Parameter *p) const
{
	if (!external())
		return NVSDK_NGX_D3D12_DestroyParameters(p);
	return usable() ? as<PfnDestroy>(fn_destroy)(p) : NVSDK_NGX_Result_FAIL_NotInitialized;
}

NVSDK_NGX_Result Ngx12Api::release_feature(NVSDK_NGX_Handle *handle) const
{
	if (!external())
		return NVSDK_NGX_D3D12_ReleaseFeature(handle);
	return usable() ? as<PfnRelease>(fn_release)(handle) : NVSDK_NGX_Result_FAIL_NotInitialized;
}

NVSDK_NGX_Result Ngx12Api::create_dlss(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle **out,
	NVSDK_NGX_Parameter *params, NVSDK_NGX_DLSS_Create_Params *create) const
{
	const ApiScope scope(this);
	return NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, out, params, create);
}

NVSDK_NGX_Result Ngx12Api::evaluate_dlss(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle *handle,
	NVSDK_NGX_Parameter *params, NVSDK_NGX_D3D12_DLSS_Eval_Params *eval) const
{
	const ApiScope scope(this);
	return NGX_D3D12_EVALUATE_DLSS_EXT(cmd, handle, params, eval);
}

namespace detail {

NVSDK_NGX_Result ngx12_route_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Feature feature,
	NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out)
{
	const Ngx12Api *api = t_api;
	if (api == nullptr || !api->external())
		return NVSDK_NGX_D3D12_CreateFeature(cmd, feature, params, out);
	if (!api->usable())
		return NVSDK_NGX_Result_FAIL_NotInitialized;
	return as<PfnCreate>(api->fn_create)(cmd, feature, params, out);
}

NVSDK_NGX_Result ngx12_route_evaluate(ID3D12GraphicsCommandList *cmd, const NVSDK_NGX_Handle *handle,
	const NVSDK_NGX_Parameter *params, PFN_NVSDK_NGX_ProgressCallback_C callback)
{
	const Ngx12Api *api = t_api;
	if (api == nullptr || !api->external())
		return NVSDK_NGX_D3D12_EvaluateFeature_C(cmd, handle, params, callback);
	if (!api->usable())
		return NVSDK_NGX_Result_FAIL_NotInitialized;
	return as<PfnEvaluate>(api->fn_evaluate)(cmd, handle, params, reinterpret_cast<void *>(callback));
}

}
}

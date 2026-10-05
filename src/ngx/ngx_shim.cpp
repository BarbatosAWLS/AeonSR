#include <Windows.h>

namespace {

typedef long(*PfnInit)(unsigned long long, const wchar_t *, void *, int, const void *);
typedef long(*PfnCreate)(void *, int, void *, void **);
typedef long(*PfnEvaluate)(void *, void *, void *, void *);
typedef long(*PfnHandle)(void *);
typedef long(*PfnRequirements)(void *, const void *, void *);

constexpr long kCallFailed = static_cast<long>(0xBAD00000);

}

#pragma optimize("", off)

extern "C" {

__declspec(dllexport) long AeonNgxInit(PfnInit fn, unsigned long long app_id,
	const wchar_t *data_path, void *device, int sdk_version, const void *params)
{
	if (fn == nullptr)
		return kCallFailed;
	volatile long result = fn(app_id, data_path, device, sdk_version, params);
	return result;
}

__declspec(dllexport) long AeonNgxCreate(PfnCreate fn, void *cmd_list, int feature_id,
	void *params, void **out_handle)
{
	if (fn == nullptr)
		return kCallFailed;
	volatile long result = fn(cmd_list, feature_id, params, out_handle);
	return result;
}

__declspec(dllexport) long AeonNgxEvaluate(PfnEvaluate fn, void *cmd_list, void *handle,
	void *params, void *callback)
{
	if (fn == nullptr)
		return kCallFailed;
	volatile long result = fn(cmd_list, handle, params, callback);
	return result;
}

__declspec(dllexport) long AeonNgxRelease(PfnHandle fn, void *handle)
{
	if (fn == nullptr)
		return kCallFailed;
	volatile long result = fn(handle);
	return result;
}

__declspec(dllexport) long AeonNgxFeatureRequirements(PfnRequirements fn, void *adapter,
	const void *discovery, void *out_requirement)
{
	if (fn == nullptr)
		return kCallFailed;
	volatile long result = fn(adapter, discovery, out_requirement);
	return result;
}

__declspec(dllexport) long AeonNgxShutdown(PfnHandle fn, void *device)
{
	if (fn == nullptr)
		return kCallFailed;
	volatile long result = fn(device);
	return result;
}

}

#pragma optimize("", on)

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
	return TRUE;
}

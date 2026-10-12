#include "aeon_sr/core/gpu_vendor.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace aeon_sr {
namespace {

bool fill_from_adapter(IDXGIAdapter *adapter, GpuInfo *out)
{
	DXGI_ADAPTER_DESC desc{};
	if (adapter == nullptr || FAILED(adapter->GetDesc(&desc)))
		return false;
	out->vendor_id = desc.VendorId;
	out->device_id = desc.DeviceId;
	out->name = desc.Description;
	out->reported_vendor_id = desc.VendorId;
	out->reported_name = desc.Description;
	const KernelAdapterIds kernel = kernel_adapter_ids(desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart);
	const VendorVerdict verdict = resolve_vendor_with_kernel(desc.VendorId, out->name, kernel);
	out->vendor = verdict.vendor;
	out->vendor_id_disputed = verdict.id_disputed;
	out->spoofed = verdict.spoofed;
	if (verdict.spoofed) {
		out->vendor_id = kernel.vendor_id;
		out->device_id = kernel.device_id;
		if (!kernel.name.empty())
			out->name = kernel.name;
	}
	out->queried = true;
	return true;
}

template <typename Fn>
Fn kmt(const char *name)
{
	HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
	if (gdi == nullptr)
		gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	return gdi != nullptr ? reinterpret_cast<Fn>(GetProcAddress(gdi, name)) : nullptr;
}

}

KernelAdapterIds kernel_adapter_ids(uint32_t luid_low, int32_t luid_high)
{
	using PfnOpen = NTSTATUS(APIENTRY *)(D3DKMT_OPENADAPTERFROMLUID *);
	using PfnQuery = NTSTATUS(APIENTRY *)(const D3DKMT_QUERYADAPTERINFO *);
	using PfnClose = NTSTATUS(APIENTRY *)(const D3DKMT_CLOSEADAPTER *);
	static const PfnOpen open = kmt<PfnOpen>("D3DKMTOpenAdapterFromLuid");
	static const PfnQuery query = kmt<PfnQuery>("D3DKMTQueryAdapterInfo");
	static const PfnClose close = kmt<PfnClose>("D3DKMTCloseAdapter");
	KernelAdapterIds out;
	if (open == nullptr || query == nullptr || close == nullptr)
		return out;
	D3DKMT_OPENADAPTERFROMLUID o{};
	o.AdapterLuid.LowPart = luid_low;
	o.AdapterLuid.HighPart = luid_high;
	if (open(&o) != 0)
		return out;
	D3DKMT_QUERY_DEVICE_IDS ids{};
	ids.PhysicalAdapterIndex = 0;
	D3DKMT_QUERYADAPTERINFO q{};
	q.hAdapter = o.hAdapter;
	q.Type = KMTQAITYPE_PHYSICALADAPTERDEVICEIDS;
	q.pPrivateDriverData = &ids;
	q.PrivateDriverDataSize = sizeof(ids);
	if (query(&q) == 0 && ids.DeviceIds.VendorID != 0) {
		out.known = true;
		out.vendor_id = ids.DeviceIds.VendorID;
		out.device_id = ids.DeviceIds.DeviceID;
		D3DKMT_ADAPTERREGISTRYINFO reg{};
		q.Type = KMTQAITYPE_ADAPTERREGISTRYINFO;
		q.pPrivateDriverData = &reg;
		q.PrivateDriverDataSize = sizeof(reg);
		if (query(&q) == 0) {
			reg.AdapterString[MAX_PATH - 1] = L'\0';
			out.name = reg.AdapterString;
		}
	}
	D3DKMT_CLOSEADAPTER c{};
	c.hAdapter = o.hAdapter;
	close(&c);
	return out;
}

bool query_gpu_info(ID3D11Device *device, GpuInfo *out)
{
	if (device == nullptr || out == nullptr)
		return false;
	IDXGIDevice *dxgi_device = nullptr;
	if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device))) ||
		dxgi_device == nullptr)
		return false;
	IDXGIAdapter *adapter = nullptr;
	const bool got = SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && fill_from_adapter(adapter, out);
	if (adapter != nullptr)
		adapter->Release();
	dxgi_device->Release();
	return got;
}

bool query_gpu_info(ID3D12Device *device, GpuInfo *out)
{
	if (device == nullptr || out == nullptr)
		return false;

	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) ||
		factory == nullptr)
		return false;
	const LUID luid = device->GetAdapterLuid();
	bool got = false;
	for (UINT i = 0; !got; ++i) {
		IDXGIAdapter *a = nullptr;
		if (factory->EnumAdapters(i, &a) != S_OK || a == nullptr)
			break;
		DXGI_ADAPTER_DESC d{};
		if (SUCCEEDED(a->GetDesc(&d)) &&
			d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart)
			got = fill_from_adapter(a, out);
		a->Release();
	}
	factory->Release();
	return got;
}

}

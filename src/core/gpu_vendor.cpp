#include "aeon_sr/core/gpu_vendor.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

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
	out->vendor = resolve_vendor(desc.VendorId, out->name, &out->vendor_id_disputed);
	out->queried = true;
	return true;
}

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

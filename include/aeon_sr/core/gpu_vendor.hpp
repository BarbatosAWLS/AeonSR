#pragma once

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D12Device;

namespace aeon_sr {

enum class GpuVendor : unsigned int {
	Unknown = 0,
	Nvidia = 1,
	Amd = 2,
	Intel = 3,
};

inline constexpr uint32_t kVendorIdNvidia = 0x10DEu;
inline constexpr uint32_t kVendorIdAmd = 0x1002u;
inline constexpr uint32_t kVendorIdAmdAti = 0x1022u;
inline constexpr uint32_t kVendorIdIntel = 0x8086u;

inline GpuVendor vendor_from_id(uint32_t vendor_id) noexcept
{
	switch (vendor_id) {
	case kVendorIdNvidia: return GpuVendor::Nvidia;
	case kVendorIdAmd:
	case kVendorIdAmdAti: return GpuVendor::Amd;
	case kVendorIdIntel: return GpuVendor::Intel;
	default: return GpuVendor::Unknown;
	}
}

inline GpuVendor vendor_from_name(const std::wstring &name) noexcept
{
	std::wstring lower;
	lower.reserve(name.size());
	for (const wchar_t c : name)
		lower.push_back(c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c);
	const auto has = [&lower](const wchar_t *needle) {
		return lower.find(needle) != std::wstring::npos;
	};
	if (has(L"nvidia") || has(L"geforce") || has(L"quadro") || has(L"tesla"))
		return GpuVendor::Nvidia;
	if (has(L"amd") || has(L"radeon") || has(L"ati "))
		return GpuVendor::Amd;
	if (has(L"intel"))
		return GpuVendor::Intel;
	return GpuVendor::Unknown;
}

inline GpuVendor resolve_vendor(uint32_t vendor_id, const std::wstring &name,
	bool *out_disputed = nullptr) noexcept
{
	const GpuVendor by_id = vendor_from_id(vendor_id);
	const GpuVendor by_name = vendor_from_name(name);
	const bool disputed = by_name != GpuVendor::Unknown && by_name != by_id;
	if (out_disputed != nullptr)
		*out_disputed = disputed;
	return disputed ? by_name : by_id;
}

inline const char *vendor_label(GpuVendor v) noexcept
{
	switch (v) {
	case GpuVendor::Nvidia: return "NVIDIA";
	case GpuVendor::Amd: return "AMD";
	case GpuVendor::Intel: return "Intel";
	case GpuVendor::Unknown: break;
	}
	return "unknown";
}

inline bool vendor_may_be_nvidia(GpuVendor v) noexcept
{
	return v == GpuVendor::Nvidia || v == GpuVendor::Unknown;
}

struct KernelAdapterIds {
	bool known = false;
	uint32_t vendor_id = 0;
	uint32_t device_id = 0;
	std::wstring name;
};

struct VendorVerdict {
	GpuVendor vendor = GpuVendor::Unknown;
	bool id_disputed = false;
	bool spoofed = false;
};

inline VendorVerdict resolve_vendor_with_kernel(uint32_t dxgi_vendor_id, const std::wstring &dxgi_name,
	const KernelAdapterIds &kernel) noexcept
{
	VendorVerdict v;
	const GpuVendor by_kernel = kernel.known ? vendor_from_id(kernel.vendor_id) : GpuVendor::Unknown;
	if (by_kernel == GpuVendor::Unknown) {
		v.vendor = resolve_vendor(dxgi_vendor_id, dxgi_name, &v.id_disputed);
		return v;
	}
	v.vendor = by_kernel;
	const GpuVendor by_name = vendor_from_name(dxgi_name);
	v.spoofed = dxgi_vendor_id != kernel.vendor_id || (by_name != GpuVendor::Unknown && by_name != by_kernel);
	return v;
}

struct GpuInfo {
	GpuVendor vendor = GpuVendor::Unknown;
	uint32_t vendor_id = 0;
	uint32_t device_id = 0;
	std::wstring name;
	bool queried = false;
	bool vendor_id_disputed = false;
	bool spoofed = false;
	uint32_t reported_vendor_id = 0;
	std::wstring reported_name;

	bool may_be_nvidia() const noexcept { return vendor_may_be_nvidia(vendor); }
};

KernelAdapterIds kernel_adapter_ids(uint32_t luid_low, int32_t luid_high);

bool query_gpu_info(ID3D11Device *device, GpuInfo *out);
bool query_gpu_info(ID3D12Device *device, GpuInfo *out);

}

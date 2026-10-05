#include "aeon_sr/ngx/ngx_dlssnr.hpp"

#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/core/gpu_vendor.hpp"
#include "aeon_sr/ngx/neural_arch_hook.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <excpt.h>

#pragma comment(lib, "bcrypt.lib")

#ifndef CUDA_VERSION
typedef unsigned long long CUtexObject;
#endif

#include <nvsdk_ngx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace aeon_sr {
namespace {

constexpr unsigned long long kAppId = 231313132ull;

constexpr long kNgxFail = static_cast<long>(0xBAD00000);

bool ngx_ok(long r) noexcept
{
	return (static_cast<unsigned long>(r) & 0xFFF00000u) != 0xBAD00000u;
}

std::wstring nr_shim_path(const std::wstring &dir)
{
	return join_path(join_path(dir, L"ngxshim"), L"nvngx.dll");
}

std::wstring directory_part(const std::wstring &path)
{
	const size_t cut = path.find_last_of(L"\\/");
	return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

std::wstring file_part(const std::wstring &path)
{
	const size_t cut = path.find_last_of(L"\\/");
	return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

const wchar_t *result_name(long r)
{
	return ngx_result_name(static_cast<NVSDK_NGX_Result>(r));
}

std::wstring format_result(const wchar_t *what, long r)
{
	return ngx_format_result(what, static_cast<NVSDK_NGX_Result>(r));
}

struct NeuralParams final : NVSDK_NGX_Parameter {
	enum Kind { kULL, kFloat, kDouble, kUInt, kInt, kPointer };
	struct Value {
		Kind kind = kInt;
		unsigned long long ull = 0;
		float f = 0.0f;
		double d = 0.0;
		unsigned int ui = 0;
		int i = 0;
		void *ptr = nullptr;
	};
	std::map<std::string, Value> values;

	void put(const char *name, const Value &v)
	{
		if (name != nullptr)
			values[name] = v;
	}
	const Value *find(const char *name) const
	{
		if (name == nullptr)
			return nullptr;
		const auto it = values.find(name);
		return it == values.end() ? nullptr : &it->second;
	}

	void Unset(const char *name)
	{
		if (name != nullptr)
			values.erase(name);
	}

	void Set(const char *n, unsigned long long v) override { Value x; x.kind = kULL; x.ull = v; put(n, x); }
	void Set(const char *n, float v) override { Value x; x.kind = kFloat; x.f = v; put(n, x); }
	void Set(const char *n, double v) override { Value x; x.kind = kDouble; x.d = v; put(n, x); }
	void Set(const char *n, unsigned int v) override { Value x; x.kind = kUInt; x.ui = v; put(n, x); }
	void Set(const char *n, int v) override { Value x; x.kind = kInt; x.i = v; put(n, x); }
	void Set(const char *n, ID3D11Resource *v) override { Value x; x.kind = kPointer; x.ptr = v; put(n, x); }
	void Set(const char *n, ID3D12Resource *v) override { Value x; x.kind = kPointer; x.ptr = v; put(n, x); }
	void Set(const char *n, void *v) override { Value x; x.kind = kPointer; x.ptr = v; put(n, x); }

	NVSDK_NGX_Result Get(const char *n, unsigned long long *o) const override
	{
		const Value *v = find(n);
		if (v == nullptr || o == nullptr) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
		*o = v->kind == kULL ? v->ull : static_cast<unsigned long long>(v->ui);
		return NVSDK_NGX_Result_Success;
	}
	NVSDK_NGX_Result Get(const char *n, float *o) const override
	{
		const Value *v = find(n);
		if (v == nullptr || o == nullptr) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
		*o = v->kind == kFloat ? v->f : static_cast<float>(v->d);
		return NVSDK_NGX_Result_Success;
	}
	NVSDK_NGX_Result Get(const char *n, double *o) const override
	{
		const Value *v = find(n);
		if (v == nullptr || o == nullptr) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
		*o = v->kind == kDouble ? v->d : static_cast<double>(v->f);
		return NVSDK_NGX_Result_Success;
	}
	NVSDK_NGX_Result Get(const char *n, unsigned int *o) const override
	{
		const Value *v = find(n);
		if (v == nullptr || o == nullptr) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
		*o = v->kind == kUInt ? v->ui : static_cast<unsigned int>(v->i);
		return NVSDK_NGX_Result_Success;
	}
	NVSDK_NGX_Result Get(const char *n, int *o) const override
	{
		const Value *v = find(n);
		if (v == nullptr || o == nullptr) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
		*o = v->kind == kInt ? v->i : static_cast<int>(v->ui);
		return NVSDK_NGX_Result_Success;
	}
	NVSDK_NGX_Result Get(const char *n, ID3D11Resource **o) const override
	{
		const Value *v = find(n);
		if (v == nullptr || o == nullptr) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
		*o = static_cast<ID3D11Resource *>(v->ptr);
		return NVSDK_NGX_Result_Success;
	}
	NVSDK_NGX_Result Get(const char *n, ID3D12Resource **o) const override
	{
		const Value *v = find(n);
		if (v == nullptr || o == nullptr) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
		*o = static_cast<ID3D12Resource *>(v->ptr);
		return NVSDK_NGX_Result_Success;
	}
	NVSDK_NGX_Result Get(const char *n, void **o) const override
	{
		const Value *v = find(n);
		if (v == nullptr || o == nullptr) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
		*o = v->ptr;
		return NVSDK_NGX_Result_Success;
	}
	void Reset() override { values.clear(); }
};

bool read_runtime_min_driver(const std::wstring &path, uint32_t *out_major, uint32_t *out_minor)
{
	if (out_major == nullptr || out_minor == nullptr)
		return false;
	*out_major = 0;
	*out_minor = 0;

	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	constexpr DWORD kScanBytes = 4u * 1024u * 1024u;
	std::vector<unsigned char> buf(kScanBytes);
	DWORD read = 0;
	const BOOL ok = ReadFile(file, buf.data(), kScanBytes, &read, nullptr);
	CloseHandle(file);
	if (!ok || read < 64)
		return false;
	buf.resize(read);

	static const unsigned char kPrefix[] = {
		0x48, 0x85, 0xC9, 0x74, 0x16, 0x85, 0xD2, 0x74, 0x06, 0xC7, 0x01 };
	static const unsigned char kMid[] = { 0x83, 0xFA, 0x01, 0x76, 0x07, 0xC7, 0x41, 0x04 };

	for (size_t i = 0; i + sizeof(kPrefix) + 4 + sizeof(kMid) + 4 <= buf.size(); ++i) {
		if (memcmp(buf.data() + i, kPrefix, sizeof(kPrefix)) != 0)
			continue;
		const size_t major_at = i + sizeof(kPrefix);
		const size_t mid_at = major_at + 4;
		if (memcmp(buf.data() + mid_at, kMid, sizeof(kMid)) != 0)
			continue;
		uint32_t major = 0, minor = 0;
		memcpy(&major, buf.data() + major_at, 4);
		memcpy(&minor, buf.data() + mid_at + sizeof(kMid), 4);
		if (major < 100u || major > 9999u || minor > 99u)
			continue;
		*out_major = major;
		*out_minor = minor;
		return true;
	}
	return false;
}

std::string sha256_hex(const char *bytes, size_t size)
{
	BCRYPT_ALG_HANDLE alg = nullptr;
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
		return {};
	std::string out;
	BCRYPT_HASH_HANDLE hash = nullptr;
	if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
		bool ok = true;
		for (size_t off = 0; ok && off < size;) {
			const ULONG chunk = static_cast<ULONG>(std::min<size_t>(size - off, size_t{ 1 } << 30));
			ok = BCRYPT_SUCCESS(BCryptHashData(hash,
				reinterpret_cast<PUCHAR>(const_cast<char *>(bytes + off)), chunk, 0));
			off += chunk;
		}
		unsigned char digest[32]{};
		if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof digest, 0))) {
			char hex[65]{};
			for (int i = 0; i < 32; ++i)
				snprintf(hex + 2 * i, 3, "%02x", digest[i]);
			out = hex;
		}
		BCryptDestroyHash(hash);
	}
	BCryptCloseAlgorithmProvider(alg, 0);
	return out;
}

bool read_runtime_file(const std::wstring &path, NeuralRuntimeKernels *out_kernels,
	NeuralRuntimeIdentity *out_identity, std::string *out_sha256)
{
	*out_kernels = NeuralRuntimeKernels{};
	*out_identity = NeuralRuntimeIdentity{};
	out_sha256->clear();

	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER size{};
	bool got = false;
	if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= (512ll << 20)) {
		const size_t n = static_cast<size_t>(size.QuadPart);
		std::vector<char> buf;
		try {
			buf.resize(n);
		} catch (const std::bad_alloc &) {
			CloseHandle(file);
			return false;
		}
		size_t off = 0;
		bool read_ok = true;
		while (read_ok && off < n) {
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n - off, size_t{ 1 } << 24));
			DWORD read = 0;
			read_ok = ReadFile(file, buf.data() + off, chunk, &read, nullptr) != 0 && read == chunk;
			off += read;
		}
		if (read_ok) {
			*out_kernels = neural_kernels_in(buf.data(), n);
			*out_identity = neural_identity_in(buf.data(), n);
			*out_sha256 = sha256_hex(buf.data(), n);
			got = true;
		}
	}
	CloseHandle(file);
	return got;
}

bool kernels_name_card(const NeuralRuntimeKernels &k, uint32_t architecture)
{
	const bool named = !k.containers.empty() || !k.text_targets.empty() || k.text_ptx_floor != 0u;
	return named && neural_capability_for_architecture(architecture) != 0u &&
		neural_kernels_serve(k, architecture);
}

bool warped_size_for(uint32_t w, uint32_t h, float scale, unsigned int mode, uint32_t *out_w,
	uint32_t *out_h) noexcept
{
	if (clamp_neural_mode(mode) == 0u || w == 0u || h == 0u)
		return false;
	const uint32_t mw = neural_mode_width(w, scale, mode);
	if (mw == 0u || mw >= neural_mode_width(w, scale, 0u))
		return false;
	uint32_t mh = static_cast<uint32_t>(std::lround(static_cast<double>(mw) * h / w)) & ~1u;
	if (mh < 64u)
		mh = 64u;
	*out_w = mw;
	*out_h = mh > h ? (h & ~1u) : mh;
	return true;
}

void model_size_for(uint32_t w, uint32_t h, float scale, uint32_t *out_w, uint32_t *out_h) noexcept
{
	if (!(scale > 0.0f) || !(scale < 1.0f)) {
		*out_w = w;
		*out_h = h;
		return;
	}
	auto dim = [](uint32_t v, float s) -> uint32_t {
		const long r = std::lround(static_cast<double>(v) * static_cast<double>(s));
		uint32_t u = r > 0 ? static_cast<uint32_t>(r) : 0u;
		u &= ~1u;
		return u < 64u ? 64u : u;
	};
	const uint32_t mw = neural_model_width(w, scale);
	uint32_t mh = h != 0u && w != 0u
		? static_cast<uint32_t>(std::lround(static_cast<double>(mw) *
			static_cast<double>(h) / static_cast<double>(w)))
		: dim(h, scale);
	mh &= ~1u;
	if (mh < 64u)
		mh = 64u;
	if (mh > h)
		mh = h & ~1u;

	*out_w = mw;
	*out_h = mh;
}

void set_create_params(NeuralParams *p, uint32_t w, uint32_t h, const NeuralRenderParams &np)
{
	p->Set(dlssnr_param::kWidth, static_cast<unsigned int>(w));
	p->Set(dlssnr_param::kHeight, static_cast<unsigned int>(h));
	p->Set(dlssnr_param::kScalingRatio, 1.0f);
	p->Set(dlssnr_param::kStyle, clamp_neural_style(np.style));
	p->Set(dlssnr_param::kRenderPreset, kNeuralRenderPresetShipping);
	p->Set(dlssnr_param::kEnabled, 1);
	p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
	p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
}

void set_eval_params(NeuralParams *p, uint32_t w, uint32_t h,
	const NeuralRenderParams &np, float mv_scale_x, float mv_scale_y)
{
	set_create_params(p, w, h, np);
	p->Set(dlssnr_param::kReset, np.reset ? 1 : 0);

	p->Set(dlssnr_param::kIntensity, clamp_neural_unit(np.intensity, kNeuralIntensityDefault, kNeuralUnitMax));
	p->Set(dlssnr_param::kLocalStructureStrength, clamp_neural_unit(np.local_structure, kNeuralStructureDefault, kNeuralExtendedMax));
	p->Set(dlssnr_param::kLocalToneStrength, clamp_neural_unit(np.local_tone, kNeuralToneDefault, kNeuralExtendedMax));

	p->Set(dlssnr_param::kSkinStructureStrength, clamp_neural_skin(np.skin_structure));

	p->Set(dlssnr_param::kUICorrection, np.ui_correction ? 1 : 0);

	p->Set(dlssnr_param::kUseAutoMask, np.auto_mask ? 1 : 0);

	p->Set(dlssnr_param::kMVecScaleX, mv_scale_x);
	p->Set(dlssnr_param::kMVecScaleY, mv_scale_y);
	p->Set(dlssnr_param::kDepthInverted, np.depth_inverted ? 1 : 0);
	const unsigned int ux = 0u, uy = 0u;
	const unsigned int uw = static_cast<unsigned int>(w);
	const unsigned int ubh = static_cast<unsigned int>(h);

	p->Set(dlssnr_param::kColorSubrectBaseX, ux);
	p->Set(dlssnr_param::kColorSubrectBaseY, uy);
	p->Set(dlssnr_param::kColorSubrectWidth, uw);
	p->Set(dlssnr_param::kColorSubrectHeight, ubh);
	p->Set(dlssnr_param::kOutputSubrectBaseX, ux);
	p->Set(dlssnr_param::kOutputSubrectBaseY, uy);
	p->Set(dlssnr_param::kOutputSubrectWidth, uw);
	p->Set(dlssnr_param::kOutputSubrectHeight, ubh);
	p->Set(dlssnr_param::kMVecSubrectBaseX, ux);
	p->Set(dlssnr_param::kMVecSubrectBaseY, uy);
	p->Set(dlssnr_param::kMVecSubrectWidth, uw);
	p->Set(dlssnr_param::kMVecSubrectHeight, ubh);
	p->Set(dlssnr_param::kDepthSubrectBaseX, ux);
	p->Set(dlssnr_param::kDepthSubrectBaseY, uy);
	p->Set(dlssnr_param::kDepthSubrectWidth, uw);
	p->Set(dlssnr_param::kDepthSubrectHeight, ubh);
}

typedef long(*PfnViaInit)(void *, unsigned long long, const wchar_t *, void *, int, const void *);
typedef long(*PfnViaCreate)(void *, void *, int, void *, void **);
typedef long(*PfnViaEvaluate)(void *, void *, void *, void *, void *);
typedef long(*PfnViaHandle)(void *, void *);
typedef long(*PfnViaRequirements)(void *, void *, const void *, void *);

long call_init(PfnViaInit via, void *fn, unsigned long long app, const wchar_t *path,
	void *device, int sdk, unsigned long *seh)
{
	__try {
		return via(fn, app, path, device, sdk, nullptr);
	} __except ((*seh = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return kNgxFail;
	}
}

long call_create(PfnViaCreate via, void *fn, void *cmd, int id, void *params,
	void **handle, unsigned long *seh)
{
	__try {
		return via(fn, cmd, id, params, handle);
	} __except ((*seh = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return kNgxFail;
	}
}

long call_evaluate(PfnViaEvaluate via, void *fn, void *cmd, void *handle, void *params,
	unsigned long *seh)
{
	__try {
		return via(fn, cmd, handle, params, nullptr);
	} __except ((*seh = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return kNgxFail;
	}
}

long call_handle(PfnViaHandle via, void *fn, void *arg, unsigned long *seh)
{
	__try {
		return via(fn, arg);
	} __except ((*seh = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return kNgxFail;
	}
}

long call_requirements(PfnViaRequirements via, void *fn, void *adapter, const void *info,
	void *out, unsigned long *seh)
{
	__try {
		return via(fn, adapter, info, out);
	} __except ((*seh = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return kNgxFail;
	}
}

IDXGIAdapter *adapter_for_device(ID3D12Device *device)
{
	if (device == nullptr)
		return nullptr;
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) ||
		factory == nullptr)
		return nullptr;
	const LUID luid = device->GetAdapterLuid();
	IDXGIAdapter *found = nullptr;
	for (UINT i = 0; found == nullptr; ++i) {
		IDXGIAdapter *a = nullptr;
		if (factory->EnumAdapters(i, &a) != S_OK || a == nullptr)
			break;
		DXGI_ADAPTER_DESC d{};
		if (SUCCEEDED(a->GetDesc(&d)) &&
			d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
			found = a;
			break;
		}
		a->Release();
	}
	factory->Release();
	return found;
}

void create_directories_w(const std::wstring &path)
{
	for (size_t i = 0; i < path.size(); ++i) {
		if (path[i] != L'\\' && path[i] != L'/')
			continue;
		if (i == 0 || path[i - 1] == L':' || path[i - 1] == L'\\' || path[i - 1] == L'/')
			continue;
		CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
	}
	CreateDirectoryW(path.c_str(), nullptr);
}

bool write_whole_file(const std::wstring &path, const void *data, size_t bytes)
{
	const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	const auto *p = static_cast<const unsigned char *>(data);
	bool ok = true;
	while (ok && bytes > 0) {

		const DWORD chunk = bytes > (1u << 30) ? (1u << 30) : static_cast<DWORD>(bytes);
		DWORD written = 0;
		ok = WriteFile(f, p, chunk, &written, nullptr) != 0 && written == chunk;
		p += chunk;
		bytes -= chunk;
	}
	CloseHandle(f);
	return ok;
}

bool record_capture_plane(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *res, D3D12_RESOURCE_STATES state, const wchar_t *file, const char *name,
	NeuralCapturePlane *out, std::wstring *err)
{
	const D3D12_RESOURCE_DESC desc = res->GetDesc();
	if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) {
		*err = std::wstring(file) + L": not a single-sample 2D texture";
		return false;
	}
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	UINT rows = 0;
	UINT64 row_bytes = 0, total = 0;
	device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
	if (rows == 0 || row_bytes == 0 || total == 0) {
		*err = std::wstring(file) + L": the texture has no copyable footprint";
		return false;
	}

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC bd{};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = total;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.Format = DXGI_FORMAT_UNKNOWN;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *buffer = nullptr;
	const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer));
	if (FAILED(hr) || buffer == nullptr) {
		wchar_t buf[96]{};
		_snwprintf_s(buf, _TRUNCATE, L": readback buffer of %llu bytes refused (0x%08lX)",
			static_cast<unsigned long long>(total), static_cast<unsigned long>(hr));
		*err = std::wstring(file) + buf;
		return false;
	}

	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = buffer;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = footprint;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = res;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;
	const bool transition = state != D3D12_RESOURCE_STATE_COMMON;
	if (transition)
		barrier12(cmd, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	if (transition)
		barrier12(cmd, res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);

	out->file = file;
	out->name = name;
	out->readback = buffer;
	out->footprint = footprint;
	out->rows = rows;
	out->row_bytes = row_bytes;
	out->total_bytes = total;
	out->width = static_cast<uint32_t>(desc.Width);
	out->height = desc.Height;
	out->format = desc.Format;
	return true;
}

void json_line(std::string *out, const char *key, const char *value, bool last = false)
{
	*out += "  \"";
	*out += key;
	*out += "\": ";
	*out += value;
	*out += last ? "\n" : ",\n";
}

std::string json_number(double v)
{
	char buf[48]{};
	snprintf(buf, sizeof buf, "%.9g", v);
	return buf;
}

std::string json_number(unsigned long long v)
{
	char buf[32]{};
	snprintf(buf, sizeof buf, "%llu", v);
	return buf;
}

const char *json_bool(bool v)
{
	return v ? "true" : "false";
}

}

bool NgxShim::load(const std::wstring &runtime_path, const std::wstring &trampoline_path,
	std::wstring *error)
{
	unload();

	HMODULE snip = LoadLibraryW(runtime_path.c_str());
	if (snip == nullptr) {
		if (error)
			*error = L"failed to load nvngx_dlssnr.dll (error " +
				std::to_wstring(static_cast<unsigned>(GetLastError())) + L")";
		return false;
	}
	HMODULE tramp = LoadLibraryW(trampoline_path.c_str());
	if (tramp == nullptr) {
		FreeLibrary(snip);
		if (error)
			*error = L"failed to load the nvngx.dll trampoline from ngxshim\\ (error " +
				std::to_wstring(static_cast<unsigned>(GetLastError())) +
				L"); the runtime only initialises when its caller is a module with that name";
		return false;
	}

	nr_runtime = snip;
	trampoline = tramp;
	fn_init = reinterpret_cast<void *>(GetProcAddress(snip, "NVSDK_NGX_D3D12_Init_Ext"));
	fn_create = reinterpret_cast<void *>(GetProcAddress(snip, "NVSDK_NGX_D3D12_CreateFeature"));
	fn_evaluate = reinterpret_cast<void *>(GetProcAddress(snip, "NVSDK_NGX_D3D12_EvaluateFeature"));
	fn_release = reinterpret_cast<void *>(GetProcAddress(snip, "NVSDK_NGX_D3D12_ReleaseFeature"));
	fn_shutdown = reinterpret_cast<void *>(GetProcAddress(snip, "NVSDK_NGX_D3D12_Shutdown1"));
	fn_requirements = reinterpret_cast<void *>(
		GetProcAddress(snip, "NVSDK_NGX_D3D12_GetFeatureRequirements"));
	fn_gpu_architecture = reinterpret_cast<void *>(GetProcAddress(snip, "NVSDK_NGX_GetGPUArchitecture"));
	via_init = reinterpret_cast<void *>(GetProcAddress(tramp, "AeonNgxInit"));
	via_create = reinterpret_cast<void *>(GetProcAddress(tramp, "AeonNgxCreate"));
	via_evaluate = reinterpret_cast<void *>(GetProcAddress(tramp, "AeonNgxEvaluate"));
	via_release = reinterpret_cast<void *>(GetProcAddress(tramp, "AeonNgxRelease"));
	via_shutdown = reinterpret_cast<void *>(GetProcAddress(tramp, "AeonNgxShutdown"));
	via_requirements = reinterpret_cast<void *>(GetProcAddress(tramp, "AeonNgxFeatureRequirements"));

	if (fn_init == nullptr || fn_create == nullptr || fn_evaluate == nullptr ||
		via_init == nullptr || via_create == nullptr || via_evaluate == nullptr) {
		if (error)
			*error = L"nvngx_dlssnr.dll or the nvngx.dll trampoline is missing entry points";
		unload();
		return false;
	}
	return true;
}

void NgxShim::unload()
{
	if (trampoline != nullptr)
		FreeLibrary(static_cast<HMODULE>(trampoline));
	if (nr_runtime != nullptr)
		FreeLibrary(static_cast<HMODULE>(nr_runtime));
	nr_runtime = nullptr;
	trampoline = nullptr;
	fn_init = fn_create = fn_evaluate = fn_release = fn_shutdown = nullptr;
	via_init = via_create = via_evaluate = via_release = via_shutdown = nullptr;
	fn_requirements = via_requirements = nullptr;
	fn_gpu_architecture = nullptr;
}

uint32_t NgxShim::minimum_architecture() const noexcept
{
	if (fn_gpu_architecture == nullptr)
		return 0u;
	return neural_constant_return(static_cast<const unsigned char *>(fn_gpu_architecture), 6u);
}

long NgxShim::init(unsigned long long app_id, const wchar_t *data_path, ID3D12Device *device,
	int sdk_version, unsigned long *seh) const
{
	if (!ready() || via_init == nullptr)
		return kNgxFail;
	return call_init(reinterpret_cast<PfnViaInit>(via_init), fn_init, app_id, data_path,
		device, sdk_version, seh);
}

long NgxShim::create(ID3D12GraphicsCommandList *cmd, int feature_id, void *params,
	void **out_handle, unsigned long *seh) const
{
	if (!ready() || via_create == nullptr)
		return kNgxFail;
	return call_create(reinterpret_cast<PfnViaCreate>(via_create), fn_create, cmd, feature_id,
		params, out_handle, seh);
}

long NgxShim::evaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params,
	unsigned long *seh) const
{
	if (!ready() || via_evaluate == nullptr)
		return kNgxFail;
	return call_evaluate(reinterpret_cast<PfnViaEvaluate>(via_evaluate), fn_evaluate, cmd,
		handle, params, seh);
}

long NgxShim::release(void *handle, unsigned long *seh) const
{
	if (!ready() || via_release == nullptr || fn_release == nullptr)
		return kNgxFail;
	return call_handle(reinterpret_cast<PfnViaHandle>(via_release), fn_release, handle, seh);
}

long NgxShim::shutdown(ID3D12Device *device, unsigned long *seh) const
{
	if (!ready() || via_shutdown == nullptr || fn_shutdown == nullptr)
		return kNgxFail;
	return call_handle(reinterpret_cast<PfnViaHandle>(via_shutdown), fn_shutdown, device, seh);
}

long NgxShim::requirements(IDXGIAdapter *adapter, const void *discovery, void *out_requirement) const
{
	if (!ready() || fn_requirements == nullptr || via_requirements == nullptr ||
		adapter == nullptr || discovery == nullptr || out_requirement == nullptr)
		return kNgxFail;
	unsigned long seh = 0;
	const long r = call_requirements(reinterpret_cast<PfnViaRequirements>(via_requirements),
		fn_requirements, adapter, discovery, out_requirement, &seh);
	return seh != 0 ? kNgxFail : r;
}

bool NeuralRenderCommon::ensure_dll_present()
{
	dll_present = false;
	shim_present = false;
	runtime_candidates = neural_runtime_candidates(addon_dir, exe_directory_w());
	if (runtime_candidates.empty()) {
		status = UpscalerStatus::MissingRuntime;
		last_error = L"nvngx_dlssnr.dll not found. It is not part of this package - NVIDIA does not\n"
			L"publish it in the DLSS SDK and the driver does not install it. Copy it from a game\n"
			L"that ships DLSS Neural Rendering into the folder holding AeonSR.addon64.";
		return false;
	}

	bool still_there = false;
	for (const std::wstring &c : runtime_candidates)
		still_there = still_there || _wcsicmp(c.c_str(), dll_path.c_str()) == 0;
	if (!still_there)
		dll_path = runtime_candidates.front();
	dll_dir = directory_part(dll_path);
	dll_present = true;
	shim_present = !shim_path().empty();
	if (!shim_present) {
		status = UpscalerStatus::MissingRuntime;
		last_error = L"ngxshim\\nvngx.dll is missing beside nvngx_dlssnr.dll. The runtime only "
			L"initialises when its caller is a module with that name, so the pass cannot run "
			L"without it - rebuild, or copy the ngxshim folder from the release.";
		return false;
	}
	return true;
}

std::wstring NeuralRenderCommon::shim_path() const
{
	for (const std::wstring *dir : { &addon_dir, &dll_dir }) {
		if (dir->empty())
			continue;
		std::wstring p = nr_shim_path(*dir);
		if (file_exists_w(p))
			return p;
	}
	return {};
}

void NeuralRenderCommon::fail(UpscalerStatus s, std::wstring detail)
{
	status = s;
	last_error = std::move(detail);
	diag_error("neural", last_error);
}

void NeuralRenderD3D12::set_command_queue(ID3D12CommandQueue *q)
{
	queue = q;
}

void NeuralRenderD3D12::wait_gpu()
{
	gpu_fence.signal_and_wait(device, queue);
}

bool NeuralRenderD3D12::init_device(ID3D12Device *dev, uint32_t w, uint32_t h)
{
	shutdown_device();

	device = dev;
	width = w;
	height = h;
	crashed = false;
	feature_unsupported = false;

	if (!ensure_dll_present())
		return false;
	if (device == nullptr) {
		fail(UpscalerStatus::InitFailed, L"null D3D12 device");
		return false;
	}

	const std::wstring appdata = local_appdata_dir_w();
	data_dir = !appdata.empty() ? appdata : join_path(addon_dir, L"AeonSR_data");
	CreateDirectoryW(data_dir.c_str(), nullptr);

	diag_info("neural", L"init begin");

	card_architecture = nvidia_architecture_id();
	runtime_targets_known = false;
	runtime_kernels = NeuralRuntimeKernels{};
	runtime_identity = NeuralRuntimeIdentity{};
	runtime_sha256.clear();
	for (size_t i = 0; i < runtime_candidates.size(); ++i) {
		const std::wstring &path = runtime_candidates[i];
		NeuralRuntimeKernels kernels;
		NeuralRuntimeIdentity identity;
		std::string sha;
		const bool known = read_runtime_file(path, &kernels, &identity, &sha);
		const bool serves = !known || card_architecture == 0u ||
			neural_kernels_serve(kernels, card_architecture);
		{
			const std::string label = known ? neural_kernels_label(kernels) : std::string("unread");
			wchar_t buf[1400]{};
			_snwprintf_s(buf, _TRUNCATE, L"runtime file %s: version %hs, %hs, sha256 %hs, %zu fatbin containers, "
				L"kernels %hs -> %hs",
				path.c_str(), identity.file_version.empty() ? "unread" : identity.file_version.c_str(),
				identity.has_certificate ? "certificate table present" : "no certificate table",
				sha.empty() ? "unread" : sha.c_str(), kernels.containers.size(),
				label.empty() ? "none named" : label.c_str(),
				serves ? "runs on this card" : "nothing for this card");
			diag_info("neural", buf);
		}
		if (i == 0 || serves) {
			dll_path = path;
			runtime_targets_known = known;
			runtime_kernels = std::move(kernels);
			runtime_identity = std::move(identity);
			runtime_sha256 = std::move(sha);
		}
		if (serves)
			break;
	}
	dll_dir = directory_part(dll_path);

	read_runtime_min_driver(dll_path, &required_driver_major, &required_driver_minor);
	nvidia_driver_version(device->GetAdapterLuid().LowPart, device->GetAdapterLuid().HighPart,
		&driver_major, &driver_minor);
	{
		wchar_t buf[160]{};
		_snwprintf_s(buf, _TRUNCATE, L"driver %u.%02u, nr_runtime requires %u.%u",
			driver_major, driver_minor, required_driver_major, required_driver_minor);
		diag_info("neural", buf);
	}
	if (driver_too_old()) {
		wchar_t buf[240]{};
		_snwprintf_s(buf, _TRUNCATE, L"nvngx_dlssnr.dll requires NVIDIA driver %u.%u or newer; this system has %u.%02u. "
			L"The nr_runtime refuses to load below its minimum, so no add-on-side workaround "
			L"applies - update the driver.",
			required_driver_major, required_driver_minor, driver_major, driver_minor);
		fail(UpscalerStatus::InitFailed, buf);
		return false;
	}

	{
		const std::string cards = neural_kernels_cards(runtime_kernels);
		wchar_t buf[400]{};
		_snwprintf_s(buf, _TRUNCATE, L"card arch 0x%03X (%hs); running %s, built for %hs", card_architecture,
			neural_arch_name(card_architecture), file_part(dll_path).c_str(),
			!runtime_targets_known ? "cards it could not be read for"
				: cards.empty() ? "no card it names" : cards.c_str());
		diag_info("neural", buf);
	}

	if (!runtime_serves_card()) {
		const std::string cards = neural_kernels_cards(runtime_kernels);
		const bool fixable = neural_allgpu_build_targets(card_architecture);
		wchar_t buf[1200]{};
		_snwprintf_s(buf, _TRUNCATE, L"%s no code for this GPU (%hs): %s is built for %hs. The neural pass is CUDA compiled "
			L"per card generation, so the runtime file is the limit here, not the driver and not "
			L"this add-on. %s Everything else in AeonSR still works on this GPU.",
			runtime_candidates.size() > 1 ? L"None of the nvngx_dlssnr*.dll files here have"
				: L"This nvngx_dlssnr.dll has",
			neural_arch_name(card_architecture), file_part(dll_path).c_str(),
			cards.empty() ? "no card it names" : cards.c_str(),
			fixable
				? L"A build of nvngx_dlssnr.dll for all GPUs runs here - put it in "
				  L"runtime\\dlss5-allgpu\\ beside the add-on, where it is tried first and an update "
				  L"never overwrites it, and switch neural rendering off and on."
				: L"No build of nvngx_dlssnr.dll is known to run on this GPU.");
		fail(UpscalerStatus::UnsupportedGpu, buf);
		return false;
	}

	std::wstring load_error;
	if (!shim.load(dll_path, shim_path(), &load_error)) {
		fail(UpscalerStatus::InitFailed, load_error);
		return false;
	}

	runtime_minimum_architecture = shim.minimum_architecture();
	neural_arch_hook_reset_stats();
	const bool hooked = neural_arch_hook_install(shim.nr_runtime);
	reported_architecture = hooked
		? neural_architecture_to_report(card_architecture, runtime_minimum_architecture,
			runtime_targets_known && kernels_name_card(runtime_kernels, card_architecture))
		: 0u;
	neural_arch_hook_report(reported_architecture);
	{
		wchar_t buf[240]{};
		_snwprintf_s(buf, _TRUNCATE, L"architecture: card 0x%03X, runtime accepts 0x%03X and newer, %s (hook %s)",
			card_architecture, runtime_minimum_architecture,
			reported_architecture != 0u ? L"telling it the card meets that" : L"telling it the truth",
			hooked ? L"in place" : L"not installed");
		diag_info("neural", buf);
	}

	{
		IDXGIAdapter *adapter = adapter_for_device(device);
		NVSDK_NGX_FeatureRequirement req{};
		const wchar_t *paths[1] = { dll_dir.c_str() };
		NVSDK_NGX_FeatureCommonInfo common{};
		common.PathListInfo.Path = paths;
		common.PathListInfo.Length = 1u;
		NVSDK_NGX_FeatureDiscoveryInfo info{};
		info.SDKVersion = NVSDK_NGX_Version_API;
		info.FeatureID = static_cast<NVSDK_NGX_Feature>(kNeuralFeatureId);
		info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
		info.Identifier.v.ApplicationId = kAppId;
		info.ApplicationDataPath = data_dir.c_str();
		info.FeatureInfo = &common;

		const long rr = shim.requirements(adapter, &info, &req);
		if (adapter != nullptr)
			adapter->Release();

		if (ngx_ok(rr)) {
			requirement_known = true;
			feature_support = static_cast<uint32_t>(req.FeatureSupported);
			min_architecture = req.MinHWArchitecture;
			wchar_t buf[240]{};
			_snwprintf_s(buf, _TRUNCATE, L"GetFeatureRequirements: support=0x%X min_arch=0x%03X",
				feature_support, min_architecture);
			diag_info("neural", buf);
		} else {
			diag_info("neural", format_result(L"GetFeatureRequirements", rr));
		}

		if (adapter_unsupported()) {
			wchar_t buf[512]{};
			_snwprintf_s(buf, _TRUNCATE, L"This GPU cannot run DLSS neural rendering. NVIDIA's own runtime was asked "
				L"before anything was started, and it answered that this adapter is below the "
				L"minimum it supports: architecture 0x%03X (%hs) or newer. That is a decision "
				L"inside nvngx_dlssnr.dll and no add-on setting changes it. Everything else in "
				L"AeonSR - the upscalers, the sharpening, the whole rest of the panel - still "
				L"works on this card; only this one pass is out.",
				min_architecture, neural_arch_name(min_architecture));
			fail(UpscalerStatus::UnsupportedGpu, buf);
			shim.unload();
			return false;
		}
	}

	unsigned long seh = 0;
	const long r = shim.init(kAppId, data_dir.c_str(), device, NVSDK_NGX_Version_API, &seh);
	diag_info("neural", format_result(L"nr_runtime Init_Ext", r));
	if (seh != 0) {
		crashed = true;
		wchar_t buf[160]{};
		_snwprintf_s(buf, _TRUNCATE, L"nr_runtime Init CRASHED (exception 0x%08lX) - neural rendering disabled", seh);
		fail(UpscalerStatus::InitFailed, buf);
		shim.unload();
		return false;
	}
	if (!ngx_ok(r)) {
		fail(UpscalerStatus::InitFailed, format_result(L"nr_runtime Init_Ext failed", r));
		shim.unload();
		return false;
	}

	params = new NeuralParams();
	initialized = true;
	status = UpscalerStatus::Idle;
	last_error.clear();

	{
		const NeuralArchHookStats hs = neural_arch_hook_stats();
		wchar_t buf[220]{};
		_snwprintf_s(buf, _TRUNCATE, L"architecture hook after init: NvAPI resolved %u, direct %u, asked %u, substituted %u",
			hs.query_resolved, hs.direct_resolved, hs.arch_asked, hs.arch_substituted);
		diag_info("neural", buf);
		if (reported_architecture != 0u && hs.arch_substituted == 0u)
			diag_state("neural", DiagLevel::Warn, "neural",
				L"the runtime did not read its architecture through the add-on, so the card is "
				L"seen as itself; this runtime build cannot be told the card meets its minimum.");
	}
	diag_info("neural", L"=== init ok ===");
	return true;
}

void NeuralRenderD3D12::destroy_feature()
{
	accum.stop();
	wait_gpu();

	if (capture.pending)
		capture_finish_now();
	for (void *&h : feature_handles) {
		if (h == nullptr)
			continue;
		if (!crashed) {
			unsigned long seh = 0;
			shim.release(h, &seh);
			if (seh != 0) {
				crashed = true;
				wchar_t buf[160]{};
				_snwprintf_s(buf, _TRUNCATE, L"ReleaseFeature CRASHED (exception 0x%08lX) - neural rendering disabled",
					seh);
				fail(UpscalerStatus::InitFailed, buf);
			}
		}
		h = nullptr;
	}
	created_passes = 0;
	requested_passes = 0;
	created_style = 0xFFFFFFFFu;
	logged_whole = 0;
	release_scratch();

}

void NeuralRenderD3D12::release_scratch()
{
	for (ID3D12Resource **r : { &color_copy, &output_tex, &frame_copy, &proxy_copy,
			&mv_small, &depth_small, &delta_tex[0], &delta_tex[1],
			&built_tex, &smooth_tex[0], &smooth_tex[1] }) {
		if (*r) { (*r)->Release(); *r = nullptr; }
	}
	depth_grid.release();
	delta_valid = false;
	last_eval_rows = 0;
	delta_index = 0;
	scratch_format = DXGI_FORMAT_UNKNOWN;
	source_format = DXGI_FORMAT_UNKNOWN;
	mv_small_format = depth_small_format = DXGI_FORMAT_UNKNOWN;
	model_width = model_height = 0;
	created_scale = 1.0f;
}

namespace {

void note_allocation(ID3D12Device *device, const wchar_t *what, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	double used = -1.0, budget = -1.0;
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter3 *adapter = nullptr;
	if (device != nullptr && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
		SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
		DXGI_QUERY_VIDEO_MEMORY_INFO info{};
		if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
			used = static_cast<double>(info.CurrentUsage) / 1048576.0;
			budget = static_cast<double>(info.Budget) / 1048576.0;
		}
	}
	if (adapter != nullptr)
		adapter->Release();
	if (factory != nullptr)
		factory->Release();
	diag_logf(DiagLevel::Info, "neural", L"creating %s %ux%u format %u; the game process holds %.0f MB of "
		L"video memory, budget %.0f MB", what, w, h, static_cast<unsigned>(fmt), used, budget);
}

}

bool NeuralRenderD3D12::ensure_composite_scratch(uint32_t w, uint32_t h, DXGI_FORMAT src_format,
	bool need_proxy)
{
	if (device == nullptr)
		return false;
	if (frame_copy == nullptr) {
		note_allocation(device, L"the full-resolution frame copy", w, h, src_format);
		if (!create_tex12(device, w, h, src_format, false, D3D12_RESOURCE_STATE_COPY_DEST, &frame_copy))
			return false;
	}
	if (need_proxy && proxy_copy == nullptr) {
		note_allocation(device, L"the proxy copy", model_width, model_height, scratch_format);
		if (!create_tex12(device, model_width, model_height, scratch_format, false, kNgxSrvState, &proxy_copy))
			return false;
	}
	return blit.ensure(device);
}

bool NeuralRenderD3D12::ensure_delta_scratch()
{
	bool noted = false;
	for (ID3D12Resource **pair : { delta_tex, smooth_tex })
		for (uint32_t i = 0; i < 2; ++i)
			if (pair[i] == nullptr) {
				if (!noted) {
					noted = true;
					note_allocation(device, L"the delta textures", model_width, model_height,
						DXGI_FORMAT_R16G16B16A16_FLOAT);
				}
				if (!create_tex12(device, model_width, model_height, DXGI_FORMAT_R16G16B16A16_FLOAT,
						false, kNgxSrvState, &pair[i]))
					return false;
			}
	if (built_tex == nullptr &&
		!create_tex12(device, model_width, model_height, DXGI_FORMAT_R16G16B16A16_FLOAT,
			false, kNgxSrvState, &built_tex))
		return false;
	if (noted)
		diag_logf(DiagLevel::Info, "neural", L"its textures are made");
	return true;
}

ID3D12Resource *NeuralRenderD3D12::downscale_guide(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src,
	ID3D12Resource **slot, DXGI_FORMAT *slot_format, const NeuralDrawConstants &c)
{
	if (src == nullptr)
		return nullptr;

	const DXGI_FORMAT want = view_format_for(src->GetDesc().Format);
	if (*slot != nullptr && *slot_format != want) {

		wait_gpu();
		(*slot)->Release();
		*slot = nullptr;
	}
	if (*slot == nullptr) {

		if (*slot_format == want)
			return nullptr;
		*slot_format = want;
		if (!create_tex12(device, model_width, model_height, want, false, kNgxSrvState, slot))
			return nullptr;
	}
	barrier12(cmd, *slot, kNgxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
	const bool ok = blit.draw_neural(device, cmd, NeuralPass::Guide, src, nullptr, nullptr, nullptr,
		*slot, want, model_width, model_height, c);
	barrier12(cmd, *slot, D3D12_RESOURCE_STATE_RENDER_TARGET, kNgxSrvState);
	return ok ? *slot : nullptr;
}

void NeuralRenderD3D12::shutdown_device()
{
	destroy_feature();

	if (initialized && device != nullptr && !crashed) {
		wait_gpu();
		unsigned long seh = 0;
		shim.shutdown(device, &seh);
	}
	initialized = false;

	blit.release();

	delete static_cast<NeuralParams *>(params);
	params = nullptr;
	reported_architecture = 0u;
	if (!crashed) {
		neural_arch_hook_remove(shim.nr_runtime);
		shim.unload();
	} else {
		neural_arch_hook_report(0u);
	}

	init_list.release();
	timer.release();
	gpu_fence.release();
	queue = nullptr;
	device = nullptr;
	width = height = 0;
	if (status != UpscalerStatus::MissingRuntime)
		status = UpscalerStatus::Idle;
}

bool NeuralRenderD3D12::ensure_feature(ID3D12GraphicsCommandList *cmd, uint32_t w, uint32_t h,
	DXGI_FORMAT color_format, DXGI_FORMAT src_format, const NeuralRenderParams &np)
{
	if (!initialized || crashed || feature_unsupported || cmd == nullptr || device == nullptr)
		return false;
	if (params == nullptr)
		return false;

	const uint32_t passes = clamp_neural_passes(np.passes);

	uint32_t mw = 0, mh = 0;
	if (!warped_size_for(w, h, np.model_scale, np.mode, &mw, &mh))
		model_size_for(w, h, np.model_scale, &mw, &mh);
	if (feature_handles[0] != nullptr && width == w && height == h && output_tex != nullptr &&
		scratch_format == color_format && source_format == src_format && requested_passes == passes &&
		created_style == clamp_neural_style(np.style) &&
		model_width == mw && model_height == mh && created_scale == np.model_scale)
		return true;

	destroy_feature();
	width = w;
	height = h;
	model_width = mw;
	model_height = mh;
	created_scale = np.model_scale;
	scratch_format = color_format;
	source_format = src_format;
	created_style = clamp_neural_style(np.style);

	auto *const p = static_cast<NeuralParams *>(params);
	p->Reset();
	set_create_params(p, mw, mh, np);

	ID3D12GraphicsCommandList *create_cmd = queue != nullptr ? init_list.begin(device) : nullptr;
	const bool own_list = create_cmd != nullptr;
	if (!own_list)
		create_cmd = cmd;

	unsigned long seh = 0;
	long cr = 0;
	uint32_t made = 0;
	for (; made < passes; ++made) {
		void *handle = nullptr;
		cr = shim.create(create_cmd, kNeuralFeatureId, p, &handle, &seh);
		{

			wchar_t model_note[64]{};
			if (mw != w || mh != h)
				_snwprintf_s(model_note, _TRUNCATE, L" model=%ux%u (%.0f%%)", mw, mh,
					static_cast<double>(created_scale) * 100.0);
			wchar_t buf[280]{};
			_snwprintf_s(buf, _TRUNCATE, L"CreateFeature %u/%u %ux%u%s src=%u fmt=%u%s style=%u preset=%u -> %s",
				made + 1u, passes, w, h, model_note, static_cast<unsigned>(src_format),
				static_cast<unsigned>(color_format),
				formats_copy_compatible(src_format, color_format) ? L"" : L" (converted)",
				created_style, 0u, result_name(cr));
			diag_info("neural", buf);
		}
		if (seh != 0 || !ngx_ok(cr) || handle == nullptr)
			break;
		feature_handles[made] = handle;
	}
	if (own_list)
		init_list.end(device, queue, gpu_fence, made > 0 && seh == 0);

	if (seh != 0) {
		crashed = true;
		wchar_t buf[160]{};
		_snwprintf_s(buf, _TRUNCATE, L"CreateFeature CRASHED (exception 0x%08lX) - neural rendering disabled", seh);
		fail(UpscalerStatus::InitFailed, buf);
		return false;
	}
	if (made == 0) {
		destroy_feature();
		if (static_cast<unsigned long>(cr) == 0xBAD00001ul && eval_count == 0) {
			feature_unsupported = true;
			const NeuralArchHookStats hs = neural_arch_hook_stats();
			wchar_t buf[900]{};
			if (reported_architecture != 0u) {
				_snwprintf_s(buf, _TRUNCATE, L"%s carries kernels for this GPU (%hs) and was told the card meets its minimum "
					L"(0x%03X), and it still refused to create the pass (FeatureNotSupported; it asked "
					L"for the architecture %u time(s), %u answered with the substitute). This build of "
					L"the runtime does not run on this card. Everything else in AeonSR works on it.",
					file_part(dll_path).c_str(), neural_arch_name(card_architecture),
					reported_architecture, hs.arch_asked, hs.arch_substituted);
			} else {
				_snwprintf_s(buf, _TRUNCATE, L"NVIDIA's neural rendering runtime refused to create the pass on this GPU "
					L"(FeatureNotSupported). This GPU reports architecture 0x%03X (%hs). "
					L"nvngx_dlssnr.dll decides inside CreateFeature which architectures it will "
					L"run on, and no setting in this add-on changes that answer - if it refuses "
					L"here, this card cannot run the neural pass with this copy of the runtime. "
					L"A newer nvngx_dlssnr.dll may answer differently. Everything else in AeonSR "
					L"works on this card.",
					card_architecture, neural_arch_name(card_architecture));
			}
			fail(UpscalerStatus::UnsupportedGpu, buf);
			return false;
		}
		fail(UpscalerStatus::InitFailed, format_result(L"CreateFeature failed", cr));
		return false;
	}
	created_passes = made;
	requested_passes = passes;

	if (!create_tex12(device, mw, mh, color_format, true, kNgxSrvState, &color_copy) ||
		!create_tex12(device, mw, mh, color_format, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output_tex)) {
		destroy_feature();
		fail(UpscalerStatus::InitFailed, L"failed to create neural scratch textures");
		return false;
	}

	if (!formats_copy_compatible(src_format, color_format) && !blit.ensure(device)) {
		destroy_feature();
		fail(UpscalerStatus::InitFailed,
			L"failed to create the neural colour conversion path (frame format differs from the "
			L"format the nr_runtime can write)");
		return false;
	}

	status = UpscalerStatus::Ready;
	last_error.clear();
	if (created_passes < passes) {
		wchar_t buf[200]{};
		_snwprintf_s(buf, _TRUNCATE, L"built %u of the %u requested passes (%s); the chain runs with %u",
			created_passes, passes, result_name(cr), created_passes);
		diag_info("neural", buf);
	}
	return true;
}

bool NeuralRenderD3D12::run(ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
	ID3D12Resource *motion_vectors, ID3D12Resource *depth,
	const NeuralRenderParams &np, float depth_jitter_u, float depth_jitter_v)
{
	if (crashed || !initialized)
		return false;

	if (cmd == nullptr || device == nullptr || color == nullptr)
		return false;

	capture_poll();

	const D3D12_RESOURCE_DESC desc = color->GetDesc();
	const uint32_t w = static_cast<uint32_t>(desc.Width);
	const uint32_t h = desc.Height;
	if (w == 0 || h == 0)
		return false;

	const DXGI_FORMAT scratch = resolve_scratch_format(desc.Format);

	DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (!ensure_feature(cmd, w, h, fmt, desc.Format, np)) {
		if (fmt == scratch || crashed)
			return false;

		diag_warn("neural", L"RGBA16F pass chain refused - falling back to the frame scratch format");
		fmt = scratch;
		if (!ensure_feature(cmd, w, h, fmt, desc.Format, np))
			return false;
	}
	if (color_copy == nullptr || output_tex == nullptr)
		return false;

	if (depth != nullptr && (depth_jitter_u != 0.0f || depth_jitter_v != 0.0f)) {
		if (ID3D12Resource *const on_grid = depth_grid.read(device, queue, gpu_fence, cmd, blit, depth,
				depth_jitter_u, depth_jitter_v))
			depth = on_grid;
	}

	const bool full_res = model_width == w && model_height == h;
	const bool passthrough = np.detail_strength == 1.0f && np.colour_strength == 1.0f &&
		np.debug_view == 0u && np.paper_white == 1.0f && np.color_space == NeuralColorSpace::Sdr;

	timer.poll();
	if (timer.last_ms > 0.0 && eval_count >= cost_logged_at + 600) {
		cost_logged_at = eval_count;
		wchar_t buf[240]{};
		_snwprintf_s(buf, _TRUNCATE, L"cost: %.2f ms of a %.2f ms frame (%.0f fps); model %ux%u, %.0f%% of it "
			L"evaluated, %u pass(es)", timer.last_ms,
			static_cast<double>(np.frame_time_ms),
			np.frame_time_ms > 0.01f ? 1000.0 / static_cast<double>(np.frame_time_ms) : 0.0,
			model_width, model_height,
			model_height != 0 && last_eval_rows != 0 ? 100.0 * last_eval_rows / model_height : 100.0,
			created_passes);
		diag_info("neural", buf);
	}
	const bool timing = timer.ensure(device, queue);
	if (timing)
		timer.begin(cmd);
	const bool ok = (full_res && passthrough && !accum.active() &&
			formats_copy_compatible(desc.Format, fmt))
		? run_fast(cmd, color, color_state, motion_vectors, depth, np)
		: run_composite(cmd, color, color_state, desc, motion_vectors, depth, np);
	if (timing)
		timer.end(cmd);
	return ok;
}

bool NeuralRenderD3D12::evaluate_chain(ID3D12GraphicsCommandList *cmd, uint32_t mw, uint32_t mh,
	ID3D12Resource *motion_vectors, ID3D12Resource *depth,
	const NeuralRenderParams &np)
{
	auto *const p = static_cast<NeuralParams *>(params);

	const float sx = static_cast<float>(mw);
	const float sy = static_cast<float>(mh);

	const bool capturing = capture.requested && !capture.pending;

	const uint32_t log_key = (static_cast<uint32_t>(clamp_neural_mode(np.mode)) << 8u) + created_passes + 1u;
	if (logged_whole != log_key) {
		logged_whole = log_key;
		wchar_t buf[300]{};
		if (last_warp_x > 0.0f)
			_snwprintf_s(buf, _TRUNCATE, L"warped evaluate (mode %u): model %ux%u, the whole image every frame, %.0f%% of the "
				L"Detail model's area with its middle at the Detail density; %u pass(es)",
				clamp_neural_mode(np.mode), mw, mh, 100.0 * static_cast<double>(last_area_share), created_passes);
		else
			_snwprintf_s(buf, _TRUNCATE, L"whole-image evaluate: model %ux%u, %u pass(es)",
				mw, mh, created_passes);
		diag_info("neural", buf);
	}

	const AccumStep astep = accum.step();
	const uint32_t links = astep == AccumStep::Off ? created_passes : 1u;

	bool ok = true;
	for (uint32_t pass = 0; pass < links && ok; ++pass) {
		if (pass > 0 || astep == AccumStep::Next) {
			barrier12(cmd, output_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			barrier12(cmd, color_copy, kNgxSrvState, D3D12_RESOURCE_STATE_COPY_DEST);
			cmd->CopyResource(color_copy, output_tex);
			barrier12(cmd, color_copy, D3D12_RESOURCE_STATE_COPY_DEST, kNgxSrvState);
			barrier12(cmd, output_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}

		set_eval_params(p, mw, mh, np, sx, sy);
		if (capturing)
			p->Set(dlssnr_param::kReset, 1);
		p->Set(dlssnr_param::kColor, color_copy);
		p->Set(dlssnr_param::kOutput, output_tex);

		if (motion_vectors != nullptr && np.feed_runtime_motion)
			p->Set(dlssnr_param::kMVec, motion_vectors);
		else
			p->Unset(dlssnr_param::kMVec);
		if (depth != nullptr)
			p->Set(dlssnr_param::kDepth, depth);
		else
			p->Unset(dlssnr_param::kDepth);

		unsigned long seh = 0;
		const long er = shim.evaluate(cmd, feature_handles[pass], p, &seh);
		++eval_count;
		if (ngx_ok(er) && seh == 0)
			accum.advance();

		if (seh != 0) {
			crashed = true;
			wchar_t buf[176]{};
			_snwprintf_s(buf, _TRUNCATE, L"EvaluateFeature CRASHED (exception 0x%08lX) - neural rendering disabled", seh);
			fail(UpscalerStatus::EvaluateFailed, buf);
			ok = false;
		} else if (!ngx_ok(er)) {
			fail(UpscalerStatus::EvaluateFailed, format_result(L"EvaluateFeature failed", er));
			ok = false;
		}

		if (capturing && pass == 0) {
			if (ok) {
				capture_record(cmd, motion_vectors, depth, np, sx, sy);
			} else {

				capture.requested = false;
				capture.last = L"not captured: " + last_error;
				diag_info("neural", L"capture: " + capture.last);
			}
		}
	}
	return ok;
}

std::wstring capture_burst_dir(const std::wstring &root, uint32_t index)
{
	wchar_t name[16]{};
	_snwprintf_s(name, _TRUNCATE, L"frame_%02u", index);
	return join_path(root, name);
}

void NeuralRenderD3D12::request_capture(std::wstring dir, uint32_t frames)
{
	capture.burst_root = dir;
	capture.burst_left = frames > 0u ? frames : 1u;
	capture.burst_index = 0u;
	capture.requested = true;
	capture.dir = capture.burst_left > 1u ? capture_burst_dir(dir, 0u) : std::move(dir);
	wchar_t note[96]{};
	_snwprintf_s(note, _TRUNCATE, L"requested - %u frame%s from the next neural frame on",
		capture.burst_left, capture.burst_left == 1u ? L"" : L"s");
	capture.last = note;
}

void NeuralRenderD3D12::capture_record(ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *motion_vectors, ID3D12Resource *depth,
	const NeuralRenderParams &np, float mv_scale_x, float mv_scale_y)
{
	capture.requested = false;
	capture_release();

	if (queue == nullptr) {
		capture.last = L"capture failed: no command queue to fence the readback on";
		diag_info("neural", capture.last);
		return;
	}

	auto state_of_guide = [this](ID3D12Resource *r) {
		return (r == mv_small || r == depth_small || r == depth_grid.texture())
			? kNgxSrvState : guide_state;
	};
	struct Want {
		ID3D12Resource *res;
		D3D12_RESOURCE_STATES state;
		const wchar_t *file;
		const char *name;
	};
	const Want wants[NeuralCapture::kMaxPlanes] = {
		{ color_copy, kNgxSrvState, L"color.bin", "color" },
		{ output_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"output.bin", "output" },
		{ motion_vectors, state_of_guide(motion_vectors), L"mvec.bin", "mvec" },
		{ depth, state_of_guide(depth), L"depth.bin", "depth" },
	};
	std::wstring err;
	for (const Want &w : wants) {
		if (w.res == nullptr)
			continue;
		if (!record_capture_plane(device, cmd, w.res, w.state, w.file, w.name,
				&capture.planes[capture.plane_count], &err)) {

			capture.last = L"capture failed: " + err;
			diag_info("neural", capture.last);
			if (capture.plane_count > 0) {
				capture.pending = true;
				capture.armed = false;
				capture.discard = true;
			}
			return;
		}
		++capture.plane_count;
	}

	std::string m = "{\n";
	json_line(&m, "model_width", json_number(static_cast<unsigned long long>(model_width)).c_str());
	json_line(&m, "model_height", json_number(static_cast<unsigned long long>(model_height)).c_str());
	json_line(&m, "frame_width", json_number(static_cast<unsigned long long>(width)).c_str());
	json_line(&m, "frame_height", json_number(static_cast<unsigned long long>(height)).c_str());
	json_line(&m, "frame_format", json_number(static_cast<unsigned long long>(source_format)).c_str());
	json_line(&m, "scratch_format", json_number(static_cast<unsigned long long>(scratch_format)).c_str());
	json_line(&m, "model_scale", json_number(static_cast<double>(created_scale)).c_str());
	json_line(&m, "warp_x", json_number(static_cast<double>(last_warp_x)).c_str());
	json_line(&m, "warp_y", json_number(static_cast<double>(last_warp_y)).c_str());
	json_line(&m, "mv_scale_x", json_number(static_cast<double>(mv_scale_x)).c_str());
	json_line(&m, "mv_scale_y", json_number(static_cast<double>(mv_scale_y)).c_str());
	json_line(&m, "depth_inverted", json_bool(np.depth_inverted));
	json_line(&m, "style", json_number(static_cast<unsigned long long>(clamp_neural_style(np.style))).c_str());
	json_line(&m, "intensity", json_number(static_cast<double>(
		clamp_neural_unit(np.intensity, kNeuralIntensityDefault, kNeuralUnitMax))).c_str());
	json_line(&m, "local_structure", json_number(static_cast<double>(
		clamp_neural_unit(np.local_structure, kNeuralStructureDefault, kNeuralExtendedMax))).c_str());
	json_line(&m, "local_tone", json_number(static_cast<double>(
		clamp_neural_unit(np.local_tone, kNeuralToneDefault, kNeuralExtendedMax))).c_str());
	json_line(&m, "skin_structure", json_number(static_cast<double>(
		clamp_neural_skin(np.skin_structure))).c_str());
	json_line(&m, "ui_correction", json_bool(NeuralRenderParams::ui_correction));
	json_line(&m, "auto_mask", json_bool(np.auto_mask));
	json_line(&m, "reset", json_bool(true));
	json_line(&m, "passes", json_number(static_cast<unsigned long long>(created_passes)).c_str());
	json_line(&m, "eval_index", json_number(static_cast<unsigned long long>(eval_count)).c_str());
	m += "  \"planes\": {\n";
	for (uint32_t i = 0; i < capture.plane_count; ++i) {
		const NeuralCapturePlane &pl = capture.planes[i];
		char line[320]{};
		snprintf(line, sizeof line,
			"    \"%s\": { \"file\": \"%ls\", \"dxgi_format\": %u, \"footprint_format\": %u, "
			"\"width\": %u, \"height\": %u, \"bytes_per_pixel\": %llu, \"row_bytes\": %llu }%s\n",
			pl.name, pl.file, static_cast<unsigned>(pl.format),
			static_cast<unsigned>(pl.footprint.Footprint.Format), pl.width, pl.height,
			static_cast<unsigned long long>(pl.width != 0 ? pl.row_bytes / pl.width : 0),
			static_cast<unsigned long long>(pl.row_bytes),
			i + 1 < capture.plane_count ? "," : "");
		m += line;
	}
	m += "  }\n}\n";
	capture.meta = std::move(m);

	capture.pending = true;
	capture.armed = false;
	capture.last = L"recorded - waiting for the GPU";
	diag_info("neural", L"capture recorded, " + std::to_wstring(capture.plane_count) +
		L" planes -> " + capture.dir);
}

void NeuralRenderD3D12::capture_poll()
{
	if (!capture.pending)
		return;
	if (!capture.armed) {

		if (queue == nullptr)
			return;
		if (gpu_fence.fence == nullptr) {
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu_fence.fence))))
				return;
			gpu_fence.value = 0;
		}
		const UINT64 v = ++gpu_fence.value;
		if (FAILED(queue->Signal(gpu_fence.fence, v)))
			return;
		capture.fence_value = v;
		capture.armed = true;
	}
	if (gpu_fence.fence->GetCompletedValue() >= capture.fence_value)
		capture_complete();
}

void NeuralRenderD3D12::capture_finish_now()
{
	if (!capture.pending)
		return;
	if (gpu_fence.signal_and_wait(device, queue)) {
		capture_complete();
		return;
	}
	capture_release();
	capture.last = L"capture lost: could not wait for the GPU before the pass was torn down";
	diag_info("neural", capture.last);
}

void NeuralRenderD3D12::capture_complete()
{
	if (capture.discard) {

		capture_release();
		return;
	}
	bool ok = true;
	std::wstring err;

	create_directories_w(capture.dir);
	if (!CreateDirectoryW(capture.dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
		ok = false;
		err = L"could not create " + capture.dir;
	}

	std::vector<uint8_t> rows;
	for (uint32_t i = 0; ok && i < capture.plane_count; ++i) {
		const NeuralCapturePlane &pl = capture.planes[i];
		void *mapped = nullptr;
		const D3D12_RANGE read{ 0, static_cast<SIZE_T>(pl.total_bytes) };
		if (FAILED(pl.readback->Map(0, &read, &mapped)) || mapped == nullptr) {
			ok = false;
			err = std::wstring(L"could not map the readback of ") + pl.file;
			break;
		}
		rows.resize(static_cast<size_t>(pl.row_bytes) * pl.rows);
		const auto *src = static_cast<const uint8_t *>(mapped) + pl.footprint.Offset;
		for (UINT r = 0; r < pl.rows; ++r)
			std::memcpy(rows.data() + static_cast<size_t>(r) * static_cast<size_t>(pl.row_bytes),
				src + static_cast<size_t>(r) * pl.footprint.Footprint.RowPitch,
				static_cast<size_t>(pl.row_bytes));
		const D3D12_RANGE none{ 0, 0 };
		pl.readback->Unmap(0, &none);
		if (!write_whole_file(join_path(capture.dir, pl.file), rows.data(), rows.size())) {
			ok = false;
			err = std::wstring(L"could not write ") + pl.file;
		}
	}
	if (ok && !write_whole_file(join_path(capture.dir, L"meta.json"), capture.meta.data(), capture.meta.size())) {
		ok = false;
		err = L"could not write meta.json";
	}

	const std::wstring dir = capture.dir;
	capture_release();
	if (ok && capture.burst_left > 1u) {
		--capture.burst_left;
		++capture.burst_index;
		capture.requested = true;
		capture.dir = capture_burst_dir(capture.burst_root, capture.burst_index);
		wchar_t note[128]{};
		_snwprintf_s(note, _TRUNCATE, L"frame %u of %u written; %u to go",
			capture.burst_index, capture.burst_index + capture.burst_left, capture.burst_left);
		capture.last = note;
	} else {
		capture.burst_left = 0u;
		capture.last = ok
			? (capture.burst_index > 0u
				? L"captured " + std::to_wstring(capture.burst_index + 1u) +
					L" frames to " + capture.burst_root
				: L"captured to " + dir)
			: L"capture failed: " + err;
	}
	diag_info("neural", capture.last);
}

void NeuralRenderD3D12::capture_release()
{
	for (NeuralCapturePlane &pl : capture.planes) {
		if (pl.readback != nullptr) {
			pl.readback->Release();
			pl.readback = nullptr;
		}
	}
	capture.plane_count = 0;
	capture.pending = false;
	capture.armed = false;
	capture.discard = false;
	capture.fence_value = 0;
	capture.meta.clear();
}

bool NeuralRenderD3D12::run_fast(ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
	ID3D12Resource *motion_vectors, ID3D12Resource *depth,
	const NeuralRenderParams &np)
{
	last_warp_x = last_warp_y = 0.0f;
	last_area_share = 1.0f;
	delta_valid = false;

	barrier12(cmd, color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier12(cmd, color_copy, kNgxSrvState, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->CopyResource(color_copy, color);
	barrier12(cmd, color_copy, D3D12_RESOURCE_STATE_COPY_DEST, kNgxSrvState);

	last_eval_rows = model_height;
	const bool ok = evaluate_chain(cmd, model_width, model_height, motion_vectors, depth, np);

	if (ok) {
		barrier12(cmd, output_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		cmd->CopyResource(color, output_tex);
		barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_DEST, color_state);
		barrier12(cmd, output_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		status = UpscalerStatus::Ready;
		last_error.clear();
	} else {
		barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, color_state);
	}
	return ok;
}

bool NeuralRenderD3D12::run_composite(ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *color, D3D12_RESOURCE_STATES color_state, const D3D12_RESOURCE_DESC &desc,
	ID3D12Resource *motion_vectors, ID3D12Resource *depth,
	const NeuralRenderParams &np)
{
	const uint32_t w = static_cast<uint32_t>(desc.Width);
	const uint32_t h = desc.Height;
	engine_journal_note(L"neural composite");
	const AccumStep astep = accum.step();
	const bool accumulating = astep != AccumStep::Off;
	const bool chained = created_passes > 1 || accumulating;
	if (!ensure_composite_scratch(w, h, desc.Format, chained)) {
		fail(UpscalerStatus::EvaluateFailed,
			L"failed to create the neural composite scratch (the full-resolution frame copy)");
		return false;
	}

	NeuralDrawConstants c;
	c.detail = np.detail_strength;
	c.colour = np.colour_strength;

	c.paper_white = np.paper_white > 0.0f ? np.paper_white : 1.0f;
	c.guard = np.highlight_guard;
	c.model_w = static_cast<float>(model_width);
	c.model_h = static_cast<float>(model_height);
	c.frame_w = static_cast<float>(w);
	if (clamp_neural_mode(np.mode) != 0u) {
		uint32_t bw = 0, bh = 0;
		model_size_for(w, h, np.model_scale, &bw, &bh);
		c.warp_x = bw != 0u ? static_cast<float>(model_width) / static_cast<float>(bw) : 0.0f;
		c.warp_y = bh != 0u ? static_cast<float>(model_height) / static_cast<float>(bh) : 0.0f;
		const float keep_floor = kNeuralModeKeepMin - 0.01f;
		if (!(c.warp_x >= keep_floor && c.warp_x < 0.999f && c.warp_y >= keep_floor && c.warp_y < 0.999f))
			c.warp_x = c.warp_y = 0.0f;
	}
	last_area_share = c.warp_x > 0.0f ? c.warp_x * c.warp_y : 1.0f;
	last_warp_x = c.warp_x;
	last_warp_y = c.warp_y;
	c.input_detail = input_detail;
	c.frame_h = static_cast<float>(h);
	c.hdr = np.color_space == NeuralColorSpace::Pq ? 2.0f
		: (np.color_space == NeuralColorSpace::ScrgbLinear ? 1.0f : 0.0f);
	c.debug = static_cast<float>(np.debug_view);

	c.carry = 1.0f;
	c.alpha = np.temporal_alpha;
	c.smooth_w = np.smooth_weight;
	c.carry_reject = np.carry_reject;
	c.scale_comp = np.scale_compensation;
	if (np.reset)
		delta_valid = false;
	if (!ensure_delta_scratch()) {
		fail(UpscalerStatus::EvaluateFailed, L"failed to create the neural delta textures");
		return false;
	}

	barrier12(cmd, color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	if (!accumulating || astep == AccumStep::First)
		cmd->CopyResource(frame_copy, color);
	barrier12(cmd, frame_copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	bool ok = true;
	const bool build_proxy = !accumulating || astep == AccumStep::First;
	if (build_proxy) {
		barrier12(cmd, color_copy, kNgxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
		ok = blit.draw_neural(device, cmd, NeuralPass::Proxy, frame_copy, nullptr, nullptr, nullptr,
			color_copy, scratch_format, model_width, model_height, c);
		if (ok && chained) {
			barrier12(cmd, color_copy, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			barrier12(cmd, proxy_copy, kNgxSrvState, D3D12_RESOURCE_STATE_COPY_DEST);
			cmd->CopyResource(proxy_copy, color_copy);
			barrier12(cmd, proxy_copy, D3D12_RESOURCE_STATE_COPY_DEST, kNgxSrvState);
			barrier12(cmd, color_copy, D3D12_RESOURCE_STATE_COPY_SOURCE, kNgxSrvState);
		} else {
			barrier12(cmd, color_copy, D3D12_RESOURCE_STATE_RENDER_TARGET, kNgxSrvState);
		}
		if (!ok)
			fail(UpscalerStatus::EvaluateFailed, L"failed to build the neural proxy at model resolution");
	}

	if (accumulating) {
		motion_vectors = nullptr;
		depth = nullptr;
		c.alpha = 1.0f;
		c.smooth_w = 1.0f;
	}

	ID3D12Resource *mv_use = motion_vectors;
	ID3D12Resource *depth_use = depth;
	if (ok && (model_width != w || model_height != h)) {
		NeuralDrawConstants cm = c;
		cm.guide_motion = 1.0f;
		mv_use = downscale_guide(cmd, motion_vectors, &mv_small, &mv_small_format, cm);
		depth_use = downscale_guide(cmd, depth, &depth_small, &depth_small_format, c);
	}

	last_eval_rows = model_height;

	if (ok && astep != AccumStep::Hold)
		ok = evaluate_chain(cmd, model_width, model_height, mv_use, depth_use, np);

	ID3D12Resource *shown = nullptr;
	if (ok && astep == AccumStep::Hold) {
		shown = smooth_tex[delta_index];
	} else if (ok) {
		const uint32_t next = delta_index ^ 1u;
		ID3D12Resource *const prev = delta_tex[delta_index];
		ID3D12Resource *const delta_cur = delta_tex[next];
		ID3D12Resource *const prev_shown = smooth_tex[delta_index];
		shown = smooth_tex[next];

		c.history = delta_valid && mv_use != nullptr && np.temporal_alpha < 1.0f ? 1.0f : 0.0f;

		barrier12(cmd, output_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kNgxSrvState);
		barrier12(cmd, delta_cur, kNgxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
		ok = blit.draw_neural(device, cmd, NeuralPass::Delta, output_tex, prev,
			chained ? proxy_copy : color_copy, mv_use, delta_cur, DXGI_FORMAT_R16G16B16A16_FLOAT,
			model_width, model_height, c);
		barrier12(cmd, delta_cur, D3D12_RESOURCE_STATE_RENDER_TARGET, kNgxSrvState);
		barrier12(cmd, output_tex, kNgxSrvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (!ok)
			fail(UpscalerStatus::EvaluateFailed, L"the neural delta draw failed");

		ID3D12Resource *const gain = delta_cur;

		if (ok) {
			barrier12(cmd, built_tex, kNgxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
			ok = blit.draw_neural(device, cmd, NeuralPass::Rebuild, gain, nullptr,
				chained ? proxy_copy : color_copy, nullptr, built_tex,
				DXGI_FORMAT_R16G16B16A16_FLOAT, model_width, model_height, c);
			barrier12(cmd, built_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, kNgxSrvState);
			if (!ok)
				fail(UpscalerStatus::EvaluateFailed, L"the neural rebuild draw failed");
		}

		if (ok) {
			barrier12(cmd, shown, kNgxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
			ok = blit.draw_neural(device, cmd, NeuralPass::Smooth, built_tex, prev_shown, nullptr,
				mv_use, shown, DXGI_FORMAT_R16G16B16A16_FLOAT, model_width, model_height, c);
			barrier12(cmd, shown, D3D12_RESOURCE_STATE_RENDER_TARGET, kNgxSrvState);
			if (!ok)
				fail(UpscalerStatus::EvaluateFailed, L"the neural presentation draw failed");
		}

		if (ok) {
			delta_index = next;
			delta_valid = true;
		}
	}

	if (ok) {
		barrier12(cmd, output_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		ok = blit.draw_neural(device, cmd, NeuralPass::Composite, shown, frame_copy, nullptr, nullptr,
			color, desc.Format, w, h, c);
		barrier12(cmd, color, D3D12_RESOURCE_STATE_RENDER_TARGET, color_state);
		barrier12(cmd, output_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (!ok)
			fail(UpscalerStatus::EvaluateFailed, L"the neural composite draw failed");
	} else {
		barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, color_state);
	}
	barrier12(cmd, frame_copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	if (ok) {
		status = UpscalerStatus::Ready;
		last_error.clear();
	}
	return ok;
}

}

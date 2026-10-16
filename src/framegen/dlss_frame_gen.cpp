#include "aeon_sr/framegen/vendor_frame_gen_impl.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/ngx/ngx_shared_d3d12.hpp"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include <nvsdk_ngx_params_dlssg.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace aeon_sr {
namespace {

constexpr const wchar_t *kTag = L"[framegen] ";

void identity(float m[4][4])
{
	std::memset(m, 0, sizeof(float) * 16);
	for (int i = 0; i < 4; ++i)
		m[i][i] = 1.0f;
}

class DlssFrameGenerator final : public VendorFrameGenerator {
public:
	~DlssFrameGenerator() override { shutdown(); }

	FrameGenerator kind() const noexcept override { return FrameGenerator::Dlss; }
	std::wstring version() const override { return std::wstring(); }
	void reset() noexcept override { frames_ = 0; }
	uint32_t max_generated() const noexcept override { return max_generated_; }

	VendorStep record(const VendorFrame &f, std::wstring *note) override
	{
		if (unavailable_) {
			*note = error_;
			return VendorStep::Unavailable;
		}
		if (f.color_space == VendorColor::Scrgb) {
			*note = L"DLSS Frame Generation does not take scRGB HDR frames";
			return VendorStep::Unavailable;
		}
		if (device_ != nullptr && device_ != f.device)
			shutdown();
		if (device_ == nullptr && !start(f.device)) {
			*note = error_;
			return VendorStep::Unavailable;
		}
		age_retired();
		if (handle_ != nullptr && (f.width != w_ || f.height != h_ || f.format != format_)) {
			retired_.push_back(Retired{ handle_, 0 });
			handle_ = nullptr;
		}
		if (handle_ == nullptr) {
			NVSDK_NGX_DLSSG_Create_Params cp{};
			cp.Width = f.width;
			cp.Height = f.height;
			cp.NativeBackbufferFormat = static_cast<unsigned int>(f.format);
			cp.RenderWidth = f.width;
			cp.RenderHeight = f.height;
			cp.DynamicResolutionScaling = false;
			const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSSG(f.cmd, 1, 1, &handle_, params_, &cp);
			if (NVSDK_NGX_FAILED(r) || handle_ == nullptr) {
				handle_ = nullptr;
				error_ = ngx_format_result(L"DLSS Frame Generation: CreateFeature", r);
				if (r == NVSDK_NGX_Result_FAIL_Denied || r == NVSDK_NGX_Result_FAIL_FeatureNotSupported ||
					r == NVSDK_NGX_Result_FAIL_OutOfDate || r == NVSDK_NGX_Result_FAIL_PlatformError) {
					unavailable_ = true;
					*note = error_;
					return VendorStep::Unavailable;
				}
				*note = error_;
				return VendorStep::Failed;
			}
			w_ = f.width;
			h_ = f.height;
			format_ = f.format;
			frames_ = 0;
			diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation ready, %ux%u", w_, h_);
		}

		if (f.motion_state != kInputState)
			barrier12(f.cmd, f.motion, f.motion_state, kInputState);

		const uint32_t count = std::clamp(f.generated, 1u, max_generated_);
		NVSDK_NGX_D3D12_DLSSG_Eval_Params e{};
		e.pBackbuffer = f.color;
		e.pDepth = f.depth;
		e.pMVecs = f.motion;
		e.pOutputInterpFrame = f.output;
		NVSDK_NGX_DLSSG_Opt_Eval_Params o{};
		o.multiFrameCount = count;
		o.multiFrameIndex = 1;
		identity(o.cameraViewToClip);
		identity(o.clipToCameraView);
		identity(o.clipToLensClip);
		identity(o.clipToPrevClip);
		identity(o.prevClipToClip);
		o.jitterOffset[0] = o.jitterOffset[1] = 0.0f;
		o.mvecScale[0] = static_cast<float>(f.width);
		o.mvecScale[1] = static_cast<float>(f.height);
		o.cameraPinholeOffset[0] = o.cameraPinholeOffset[1] = 0.0f;
		o.cameraPos[0] = o.cameraPos[1] = o.cameraPos[2] = 0.0f;
		o.cameraUp[0] = 0.0f;
		o.cameraUp[1] = 1.0f;
		o.cameraUp[2] = 0.0f;
		o.cameraRight[0] = 1.0f;
		o.cameraRight[1] = o.cameraRight[2] = 0.0f;
		o.cameraFwd[0] = o.cameraFwd[1] = 0.0f;
		o.cameraFwd[2] = 1.0f;
		o.cameraNear = 0.1f;
		o.cameraFar = 1000.0f;
		o.cameraFOV = 1.0471976f;
		o.cameraAspectRatio = static_cast<float>(f.width) / static_cast<float>(f.height);
		o.colorBuffersHDR = f.color_space == VendorColor::Pq;
		o.depthInverted = false;
		o.cameraMotionIncluded = true;
		o.reset = f.reset || frames_ == 0;
		o.automodeOverrideReset = false;
		o.notRenderingGameFrames = false;
		o.orthoProjection = false;
		o.motionVectorsInvalidValue = 1.0e30f;
		o.motionVectorsDilated = false;
		o.menuDetectionEnabled = false;
		const bool fresh = o.reset;
		const uint32_t evaluations = fresh ? 1u : count;
		if (fresh)
			o.multiFrameCount = 1;
		NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
		for (uint32_t i = 1; i <= evaluations && !NVSDK_NGX_FAILED(r); ++i) {
			e.pOutputInterpFrame = i == 1 ? f.output : f.more_outputs[i - 2];
			o.multiFrameIndex = i;
			if (e.pOutputInterpFrame == nullptr) {
				r = NVSDK_NGX_Result_FAIL_InvalidParameter;
				break;
			}
			r = NGX_D3D12_EVALUATE_DLSSG(f.cmd, handle_, params_, &e, &o);
		}

		if (f.motion_state != kInputState)
			barrier12(f.cmd, f.motion, kInputState, f.motion_state);
		if (NVSDK_NGX_FAILED(r)) {
			*note = ngx_format_result(L"DLSS Frame Generation: Evaluate", r);
			frames_ = 0;
			return VendorStep::Failed;
		}
		++frames_;
		return fresh ? VendorStep::Warming : VendorStep::Generated;
	}

private:
	bool start(ID3D12Device *device)
	{
		const std::wstring dll = find_vendor_runtime(L"nvngx_dlssg.dll");
		std::wstring dir = vendor_module_directory();
		if (!dll.empty())
			dir = dll.substr(0, dll.find_last_of(L"\\/"));
		std::wstring data = local_appdata_dir_w();
		if (data.empty())
			data = dir;
		CreateDirectoryW(data.c_str(), nullptr);
		const wchar_t *paths[] = { dir.c_str() };
		NVSDK_NGX_FeatureCommonInfo info{};
		info.PathListInfo.Path = paths;
		info.PathListInfo.Length = 1;
		info.LoggingInfo.LoggingCallback = ngx_log_callback;
		info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
		info.LoggingInfo.DisableOtherLoggingSinks = false;
		const NVSDK_NGX_Result r = ngx12_acquire(ngx12_static_api(), device, data.c_str(), &info, kTag);
		if (NVSDK_NGX_FAILED(r)) {
			error_ = ngx_format_result(L"DLSS Frame Generation needs an NVIDIA GeForce RTX 40 or newer; NGX did not start", r);
			unavailable_ = true;
			return false;
		}
		device_ = device;
		NVSDK_NGX_Parameter *caps = nullptr;
		if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) || caps == nullptr) {
			error_ = L"DLSS Frame Generation: NGX gave no capabilities";
			unavailable_ = true;
			shutdown();
			return false;
		}
		unsigned int available = 0, needs_driver = 0, major = 0, minor = 0;
		int init_result = 0;
		caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
		caps->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs_driver);
		caps->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &major);
		caps->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &minor);
		caps->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init_result);
		unsigned int most = 1;
		if (NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &most)) || most < 1)
			most = 1;
		max_generated_ = std::min<uint32_t>(most, kVendorMaxGenerated);
		params_ = caps;
		if (available == 0) {
			wchar_t b[256]{};
			if (needs_driver != 0)
				_snwprintf_s(b, _TRUNCATE, L"DLSS Frame Generation needs NVIDIA driver %u.%u or newer", major, minor);
			else if (dll.empty())
				_snwprintf_s(b, _TRUNCATE, L"DLSS Frame Generation needs a GeForce RTX 40 or newer and "
					L"nvngx_dlssg.dll in the add-on's runtime folder");
			else
				_snwprintf_s(b, _TRUNCATE, L"DLSS Frame Generation needs a GeForce RTX 40 or newer (%ls)",
					ngx_result_name(static_cast<NVSDK_NGX_Result>(init_result)));
			error_ = b;
			unavailable_ = true;
			shutdown();
			return false;
		}
		diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation: up to %u frames between two real ones here "
			L"(%ux)", max_generated_, max_generated_ + 1);
		return true;
	}

	void age_retired() noexcept
	{
		for (Retired &r : retired_)
			if (++r.frames > 4 && r.handle != nullptr) {
				NVSDK_NGX_D3D12_ReleaseFeature(r.handle);
				r.handle = nullptr;
			}
		while (!retired_.empty() && retired_.front().handle == nullptr)
			retired_.erase(retired_.begin());
	}

	void shutdown() noexcept
	{
		for (Retired &r : retired_)
			if (r.handle != nullptr)
				NVSDK_NGX_D3D12_ReleaseFeature(r.handle);
		retired_.clear();
		if (handle_ != nullptr)
			NVSDK_NGX_D3D12_ReleaseFeature(handle_);
		handle_ = nullptr;
		if (params_ != nullptr)
			NVSDK_NGX_D3D12_DestroyParameters(params_);
		params_ = nullptr;
		if (device_ != nullptr)
			ngx12_release(ngx12_static_api(), device_, kTag);
		device_ = nullptr;
		w_ = h_ = 0;
		format_ = DXGI_FORMAT_UNKNOWN;
		frames_ = 0;
	}

	ID3D12Device *device_ = nullptr;
	NVSDK_NGX_Parameter *params_ = nullptr;
	NVSDK_NGX_Handle *handle_ = nullptr;
	uint32_t w_ = 0, h_ = 0;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	uint64_t frames_ = 0;
	uint32_t max_generated_ = 1;
	bool unavailable_ = false;
	std::wstring error_;
	struct Retired {
		NVSDK_NGX_Handle *handle = nullptr;
		uint32_t frames = 0;
	};
	std::vector<Retired> retired_;
};

}

std::unique_ptr<VendorFrameGenerator> make_dlss_frame_generator()
{
	return std::make_unique<DlssFrameGenerator>();
}

}

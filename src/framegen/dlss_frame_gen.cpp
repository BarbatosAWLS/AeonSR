#include "aeon_sr/framegen/vendor_frame_gen_impl.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/ngx/ngx_dlssg_files.hpp"
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

NVSDK_NGX_Result create_guarded(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle **handle,
	NVSDK_NGX_Parameter *params, NVSDK_NGX_DLSSG_Create_Params *cp, DWORD *seh_code)
{
	__try {
		return NGX_D3D12_CREATE_DLSSG(cmd, 1, 1, handle, params, cp);
	} __except ((*seh_code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return NVSDK_NGX_Result_Fail;
	}
}

NVSDK_NGX_Result evaluate_guarded(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle *handle,
	NVSDK_NGX_Parameter *params, NVSDK_NGX_D3D12_DLSSG_Eval_Params *e, NVSDK_NGX_DLSSG_Opt_Eval_Params *o,
	DWORD *seh_code)
{
	__try {
		return NGX_D3D12_EVALUATE_DLSSG(cmd, handle, params, e, o);
	} __except ((*seh_code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return NVSDK_NGX_Result_Fail;
	}
}

std::wstring seh_text(DWORD code)
{
	wchar_t b[64]{};
	_snwprintf_s(b, _TRUNCATE, L" (exception 0x%08lX inside it)", code);
	return b;
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
			DWORD seh = 0;
			const NVSDK_NGX_Result r = create_guarded(f.cmd, &handle_, params_, &cp, &seh);
			if (NVSDK_NGX_FAILED(r) || handle_ == nullptr) {
				handle_ = nullptr;
				error_ = ngx_format_result(L"DLSS Frame Generation: CreateFeature", r);
				if (seh != 0)
					error_ += seh_text(seh);
				if (!refused_.empty())
					error_ = refused_ + L"; " + error_;
				if (dlssg_refusal_is_final(!refused_.empty(), static_cast<uint32_t>(r), seh)) {
					diag_logf(DiagLevel::Warn, "framegen", L"%ls", error_.c_str());
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
			diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation ready, %ux%u%ls", w_, h_, refused_.empty()
				? L"" : L", although NGX said it is not available here: CreateFeature succeeded anyway");
			if (!created_logged_) {
				created_logged_ = true;
				diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation after CreateFeature: %ls",
					describe_ngx_modules().c_str());
			}
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
		DWORD seh = 0;
		for (uint32_t i = 1; i <= evaluations && !NVSDK_NGX_FAILED(r); ++i) {
			e.pOutputInterpFrame = i == 1 ? f.output : f.more_outputs[i - 2];
			o.multiFrameIndex = i;
			if (e.pOutputInterpFrame == nullptr) {
				r = NVSDK_NGX_Result_FAIL_InvalidParameter;
				break;
			}
			r = evaluate_guarded(f.cmd, handle_, params_, &e, &o, &seh);
		}

		if (f.motion_state != kInputState)
			barrier12(f.cmd, f.motion, kInputState, f.motion_state);
		if (NVSDK_NGX_FAILED(r)) {
			*note = ngx_format_result(L"DLSS Frame Generation: Evaluate", r);
			frames_ = 0;
			if (seh != 0) {
				*note += seh_text(seh);
				error_ = *note;
				diag_logf(DiagLevel::Warn, "framegen", L"%ls", error_.c_str());
				unavailable_ = true;
				return VendorStep::Unavailable;
			}
			return VendorStep::Failed;
		}
		++frames_;
		return fresh ? VendorStep::Warming : VendorStep::Generated;
	}

private:
	bool start(ID3D12Device *device)
	{
		const std::wstring addon_dir = vendor_module_directory();
		const DlssgChoice choice = find_dlssg(addon_dir);
		std::wstring dlss_dir = find_ngx_runtime_dir(addon_dir);
		if (dlss_dir.empty())
			dlss_dir = choice.dir.empty() ? addon_dir : choice.dir;
		const NgxPathList list = ngx_path_list_for(dlss_dir, choice);
		std::wstring theirs_vs_ours;
		if (choice.source != DlssgSource::AddonRuntime && !choice.addon_file.empty())
			theirs_vs_ours = L" (the add-on's copy, " + choice.addon_file + L", is " +
				version_text(file_version_number(choice.addon_file)) + L")";
		diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation: nvngx_dlssg.dll %ls%ls%ls: %ls%ls%ls%ls",
			choice.file.empty() ? L"" : choice.file.c_str(), choice.file.empty() ? L"" : L" ",
			choice.file.empty() ? L"" : version_text(file_version_number(choice.file)).c_str(), choice.why.c_str(),
			theirs_vs_ours.c_str(), list.note.empty() ? L"" : L"; ", list.note.c_str());
		std::wstring data = local_appdata_dir_w();
		if (data.empty())
			data = dlss_dir;
		CreateDirectoryW(data.c_str(), nullptr);
		std::vector<const wchar_t *> paths;
		for (const std::wstring &d : list.dirs)
			paths.push_back(d.c_str());
		NVSDK_NGX_FeatureCommonInfo info{};
		info.PathListInfo.Path = paths.data();
		info.PathListInfo.Length = static_cast<unsigned int>(paths.size());
		info.LoggingInfo.LoggingCallback = ngx_log_callback;
		info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
		info.LoggingInfo.DisableOtherLoggingSinks = false;
		const NVSDK_NGX_Result r = ngx12_acquire(ngx12_static_api(), device, data.c_str(), &info, kTag);
		if (NVSDK_NGX_FAILED(r)) {
			error_ = ngx_format_result(L"DLSS Frame Generation needs an NVIDIA GeForce RTX 40 or newer; NGX did not start", r);
			diag_logf(DiagLevel::Warn, "framegen", L"%ls", error_.c_str());
			unavailable_ = true;
			return false;
		}
		device_ = device;
		{
			const std::vector<std::wstring> used = ngx12_instance_paths(ngx12_static_api(), device);
			std::wstring folders;
			for (const std::wstring &d : used)
				folders += (folders.empty() ? L"" : L", ") + d;
			const std::wstring from = first_holding(used, L"nvngx_dlssg.dll", file_exists_w);
			diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation: NGX %ls, started by the add-on with the "
				L"folders %ls; the first nvngx_dlssg.dll in them is %ls (what NGX really loaded is read below)%ls",
				ngx_result_name(r), folders.empty() ? L"(none)" : folders.c_str(),
				from.empty() ? L"none: NGX takes the driver's own" : from.c_str(),
				used == list.dirs ? L"" : L"; it was started before with these, so the choice above waits for its "
				L"next start");
		}
		NVSDK_NGX_Parameter *caps = nullptr;
		const NVSDK_NGX_Result cr = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
		if (NVSDK_NGX_FAILED(cr) || caps == nullptr) {
			error_ = ngx_format_result(L"DLSS Frame Generation: NGX gave no capabilities", cr);
			diag_logf(DiagLevel::Warn, "framegen", L"%ls", error_.c_str());
			if (caps != nullptr)
				NVSDK_NGX_D3D12_DestroyParameters(caps);
			unavailable_ = true;
			shutdown();
			return false;
		}
		unsigned int available = 0, needs_driver = 0, major = 0, minor = 0, most = 0;
		int init_result = 0;
		const bool has_available = !NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available));
		const bool has_driver = !NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver,
			&needs_driver));
		caps->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &major);
		caps->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &minor);
		const bool has_init = !NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult,
			&init_result));
		const bool has_most = !NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &most));
		max_generated_ = std::clamp<uint32_t>(has_most ? most : 1u, 1u, kVendorMaxGenerated);
		params_ = caps;
		{
			wchar_t b[320]{};
			_snwprintf_s(b, _TRUNCATE, L"DLSS Frame Generation: NGX says Available=%ls, FeatureInitResult=%ls "
				L"(0x%08X), MultiFrameCountMax=%ls, NeedsUpdatedDriver=%ls", has_available ? (available ? L"1" : L"0")
				: L"absent", has_init ? ngx_result_name(static_cast<NVSDK_NGX_Result>(init_result)) : L"absent",
				static_cast<unsigned>(init_result), has_most ? std::to_wstring(most).c_str() : L"absent",
				has_driver ? (needs_driver ? L"1" : L"0") : L"absent");
			std::wstring line = b;
			if (needs_driver != 0) {
				_snwprintf_s(b, _TRUNCATE, L" (MinDriverVersion %u.%u)", major, minor);
				line += b;
			}
			diag_logf(DiagLevel::Info, "framegen", L"%ls", line.c_str());
			diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation: %ls", describe_ngx_modules().c_str());
		}
		refused_.clear();
		if (available == 0) {
			wchar_t b[256]{};
			if (needs_driver != 0)
				_snwprintf_s(b, _TRUNCATE, L"DLSS Frame Generation needs NVIDIA driver %u.%u or newer", major, minor);
			else if (choice.file.empty())
				_snwprintf_s(b, _TRUNCATE, L"DLSS Frame Generation needs a GeForce RTX 40 or newer and "
					L"nvngx_dlssg.dll in the add-on's runtime folder");
			else
				_snwprintf_s(b, _TRUNCATE, L"DLSS Frame Generation needs a GeForce RTX 40 or newer (%ls)",
					ngx_result_name(static_cast<NVSDK_NGX_Result>(init_result)));
			refused_ = b;
			diag_logf(DiagLevel::Warn, "framegen", L"%ls, NGX says; CreateFeature is asked anyway, for a frame "
				L"generation unlocker that answers it", refused_.c_str());
		}
		diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation: up to %u frames between two real ones here "
			L"(%ux), from NGX's MultiFrameCountMax", max_generated_, max_generated_ + 1);
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
	std::wstring refused_;
	bool created_logged_ = false;
	struct Retired {
		NVSDK_NGX_Handle *handle = nullptr;
		uint32_t frames = 0;
	};
	std::vector<Retired> retired_;
};

}

bool dlssg_refusal_is_final(bool capability_refused, uint32_t ngx_result, uint32_t seh_code) noexcept
{
	if (capability_refused || seh_code != 0)
		return true;
	const NVSDK_NGX_Result r = static_cast<NVSDK_NGX_Result>(ngx_result);
	return r == NVSDK_NGX_Result_FAIL_Denied || r == NVSDK_NGX_Result_FAIL_FeatureNotSupported ||
		r == NVSDK_NGX_Result_FAIL_OutOfDate || r == NVSDK_NGX_Result_FAIL_PlatformError;
}

std::unique_ptr<VendorFrameGenerator> make_dlss_frame_generator()
{
	return std::make_unique<DlssFrameGenerator>();
}

}

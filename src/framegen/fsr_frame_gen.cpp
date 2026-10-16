#include "aeon_sr/framegen/vendor_frame_gen_impl.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/runtime_search.hpp"
#include "aeon_sr/upscalers/ffx_loader.hpp"

#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>

#include <atomic>
#include <cmath>
#include <thread>

namespace aeon_sr {
namespace {

constexpr const wchar_t *kDll = L"amd_fidelityfx_framegeneration_dx12.dll";

ffxReturnCode_t create_guarded(PfnFfxCreateContext fn, ffxContext *ctx, ffxCreateContextDescHeader *desc,
	bool *crashed)
{
	__try {
		return fn(ctx, desc, nullptr);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		*crashed = true;
		return FFX_API_RETURN_ERROR;
	}
}

std::wstring ffx_error(const wchar_t *what, ffxReturnCode_t r)
{
	wchar_t buf[160]{};
	_snwprintf_s(buf, _TRUNCATE, L"%s returned %u", what, static_cast<unsigned>(r));
	return buf;
}

class FsrFrameGenerator final : public VendorFrameGenerator {
public:
	~FsrFrameGenerator() override
	{
		join();
		destroy_context();
		ffx_unload(api_);
	}

	FrameGenerator kind() const noexcept override { return FrameGenerator::Fsr; }
	std::wstring version() const override { return state_.load() == kReady ? version_ : std::wstring(); }
	void reset() noexcept override { frames_ = 0; }
	bool busy() const noexcept override { return state_.load(std::memory_order_acquire) == kBuilding; }

	VendorStep record(const VendorFrame &f, std::wstring *note) override
	{
		const int s = state_.load(std::memory_order_acquire);
		if (s == kBuilding)
			return VendorStep::Building;
		if (s == kFailed) {
			*note = error_;
			return VendorStep::Unavailable;
		}
		if (s == kReady && (f.device != device_ || f.width != w_ || f.height != h_ || f.format != format_ ||
				f.color_space != color_)) {
			join();
			if (retire_ != nullptr)
				api_.DestroyContext(&retire_, nullptr);
			retire_ = context_;
			retire_frames_ = 0;
			context_ = nullptr;
			state_.store(kIdle, std::memory_order_release);
		}
		if (state_.load(std::memory_order_acquire) == kIdle) {
			start_build(f);
			return VendorStep::Building;
		}
		join();
		if (retire_ != nullptr && retire_frames_++ > 4) {
			api_.DestroyContext(&retire_, nullptr);
			retire_ = nullptr;
			retire_frames_ = 0;
		}

		const bool fresh = frames_ == 0 || f.reset;
		++frame_id_;

		ffxConfigureDescFrameGeneration cfg{};
		cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		cfg.swapChain = nullptr;
		cfg.frameGenerationEnabled = true;
		cfg.allowAsyncWorkloads = false;
		cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
		cfg.onlyPresentGenerated = false;
		cfg.generationRect = { 0, 0, static_cast<int32_t>(f.width), static_cast<int32_t>(f.height) };
		cfg.frameID = frame_id_;
		ffxReturnCode_t r = api_.Configure(&context_, &cfg.header);
		if (r != FFX_API_RETURN_OK) {
			*note = ffx_error(L"FSR frame generation: Configure", r);
			frames_ = 0;
			return VendorStep::Failed;
		}

		ffxDispatchDescFrameGenerationPrepareV2 prep{};
		prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
		prep.frameID = frame_id_;
		prep.flags = 0;
		prep.commandList = f.cmd;
		prep.renderSize = { f.width, f.height };
		prep.jitterOffset = { 0.0f, 0.0f };
		prep.motionVectorScale = { static_cast<float>(f.width), static_cast<float>(f.height) };
		prep.frameTimeDelta = static_cast<float>(f.frame_ms);
		prep.reset = fresh;
		prep.cameraNear = 0.1f;
		prep.cameraFar = 1000.0f;
		prep.cameraFovAngleVertical = 1.0471976f;
		prep.viewSpaceToMetersFactor = 1.0f;
		prep.depth = ffxApiGetResourceDX12(f.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
		prep.motionVectors = ffxApiGetResourceDX12(f.motion, f.motion_state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
			? FFX_API_RESOURCE_STATE_COMPUTE_READ : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		prep.cameraUp[1] = 1.0f;
		prep.cameraRight[0] = 1.0f;
		prep.cameraForward[2] = 1.0f;
		r = api_.Dispatch(&context_, &prep.header);
		if (r != FFX_API_RETURN_OK) {
			*note = ffx_error(L"FSR frame generation: Prepare", r);
			frames_ = 0;
			return VendorStep::Failed;
		}

		ffxDispatchDescFrameGeneration d{};
		d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
		d.commandList = f.cmd;
		d.presentColor = ffxApiGetResourceDX12(f.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
		d.outputs[0] = ffxApiGetResourceDX12(f.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		d.numGeneratedFrames = 1;
		d.reset = fresh;
		d.backbufferTransferFunction = f.color_space == VendorColor::Scrgb ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB
			: f.color_space == VendorColor::Pq ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ
			: FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		d.minMaxLuminance[0] = 0.0f;
		d.minMaxLuminance[1] = f.color_space == VendorColor::Sdr ? 100.0f : 1000.0f;
		d.generationRect = cfg.generationRect;
		d.frameID = frame_id_;
		r = api_.Dispatch(&context_, &d.header);
		if (r != FFX_API_RETURN_OK) {
			*note = ffx_error(L"FSR frame generation: Dispatch", r);
			frames_ = 0;
			return VendorStep::Failed;
		}
		return frames_++ == 0 || fresh ? VendorStep::Warming : VendorStep::Generated;
	}

private:
	enum : int { kIdle = 0, kBuilding, kReady, kFailed };

	void join()
	{
		if (builder_.joinable())
			builder_.join();
	}

	void destroy_context() noexcept
	{
		if (api_.DestroyContext != nullptr) {
			if (context_ != nullptr)
				api_.DestroyContext(&context_, nullptr);
			if (retire_ != nullptr)
				api_.DestroyContext(&retire_, nullptr);
		}
		context_ = retire_ = nullptr;
	}

	void start_build(const VendorFrame &f)
	{
		join();
		device_ = f.device;
		w_ = f.width;
		h_ = f.height;
		format_ = f.format;
		color_ = f.color_space;
		frames_ = 0;
		state_.store(kBuilding, std::memory_order_release);
		device_->AddRef();
		builder_ = std::thread([this] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			const bool ok = build();
			device_->Release();
			state_.store(ok ? kReady : kFailed, std::memory_order_release);
		});
	}

	bool build()
	{
		if (api_.module == nullptr) {
			const std::wstring path = find_vendor_runtime(kDll, kFsrPreferredSubdir);
			if (path.empty()) {
				error_ = L"FSR frame generation needs amd_fidelityfx_framegeneration_dx12.dll in the add-on's "
					L"runtime folder";
				return false;
			}
			std::wstring err;
			if (!ffx_load(api_, path, err)) {
				error_ = L"FSR frame generation: " + err;
				return false;
			}
		}
		ffxCreateBackendDX12Desc backend{};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backend.device = device_;
		ffxCreateContextDescFrameGeneration desc{};
		desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
		desc.header.pNext = &backend.header;
		desc.flags = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
			(color_ != VendorColor::Sdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0u);
		desc.displaySize = { w_, h_ };
		desc.maxRenderSize = { w_, h_ };
		desc.backBufferFormat = ffxApiGetSurfaceFormatDX12(format_);
		ffxContext ctx = nullptr;
		bool crashed = false;
		const ffxReturnCode_t r = create_guarded(api_.CreateContext, &ctx, &desc.header, &crashed);
		if (crashed) {
			error_ = L"FSR frame generation: the runtime crashed creating its context";
			return false;
		}
		if (r != FFX_API_RETURN_OK || ctx == nullptr) {
			error_ = ffx_error(L"FSR frame generation: CreateContext", r);
			return false;
		}
		context_ = ctx;
		version_.clear();
		ffxQueryGetProviderVersion pv{};
		pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		if (api_.Query(&context_, &pv.header) == FFX_API_RETURN_OK && pv.versionName != nullptr)
			for (const char *c = pv.versionName; *c != '\0'; ++c)
				version_ += static_cast<wchar_t>(static_cast<unsigned char>(*c));
		diag_logf(DiagLevel::Info, "framegen", L"FSR frame generation %ls ready, %ux%u", version_.c_str(), w_, h_);
		return true;
	}

	FfxApi api_;
	std::atomic<int> state_{ kIdle };
	std::thread builder_;
	std::wstring error_, version_;
	ffxContext context_ = nullptr;
	ffxContext retire_ = nullptr;
	uint32_t retire_frames_ = 0;
	ID3D12Device *device_ = nullptr;
	uint32_t w_ = 0, h_ = 0;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	VendorColor color_ = VendorColor::Sdr;
	uint64_t frame_id_ = 0;
	uint64_t frames_ = 0;
};

}

std::unique_ptr<VendorFrameGenerator> make_fsr_frame_generator()
{
	return std::make_unique<FsrFrameGenerator>();
}

}

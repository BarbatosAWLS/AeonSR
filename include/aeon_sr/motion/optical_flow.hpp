#pragma once

#include <d3d12.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace aeon_sr {

class OpticalFlowD3D12 {
public:
	enum class Quality : uint8_t { Balanced = 0, High = 1 };

	~OpticalFlowD3D12();

	bool ensure(ID3D12Device *device, uint32_t width, uint32_t height, std::wstring *error);

	ID3D12Device *pipelines_failed_on = nullptr;
	std::wstring pipelines_error;

	bool failed_permanently() const noexcept { return pipelines_failed_on != nullptr; }

	bool quality_ok[2] = { false, false };
	bool model_ok() const noexcept
	{
		return model_textures_ && model_state_.load(std::memory_order_acquire) == kModelReady;
	}
	bool model_failed() const noexcept
	{
		return (motion_.res != nullptr && !model_textures_) ||
			model_state_.load(std::memory_order_acquire) == kModelFailed;
	}
	std::wstring model_error() const
	{
		return (motion_.res != nullptr && !model_textures_) ? model_textures_error_ : model_build_error_;
	}
	bool wait_for_camera_model();

	uint32_t texture_retry_ms = 5000;
	uint32_t fail_textures_for_test = 0;
	uint32_t fail_model_textures_for_test = 0;
	Quality usable(Quality want) const noexcept
	{
		const uint32_t q = static_cast<uint32_t>(want);
		if (quality_ok[q])
			return want;
		return q == 0 ? Quality::High : Quality::Balanced;
	}

	bool record(ID3D12GraphicsCommandList *cmd,
		ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
		ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state,
		Quality quality, bool launcher_filters, std::wstring *error,
		bool camera_model = true,
		float jitter_x_px = 0.0f, float jitter_y_px = 0.0f,
		bool publish = true);

	void set_depth_logarithmic(bool logarithmic) noexcept { depth_logarithmic_ = logarithmic; }

	void set_colour_space(uint32_t colour_space) noexcept { colour_space_ = colour_space; }

	void reset_history() noexcept
	{
		frames_ = 0;
		prev_jitter_x_ = 0.0f;
		prev_jitter_y_ = 0.0f;
		skipped_ = 0;
		model_cold_ = true;
	}

	void release();
	void release_textures();

	bool ready() const noexcept { return device_ != nullptr && motion_.res != nullptr; }
	bool has_history() const noexcept { return frames_ >= 2; }

	ID3D12Resource *motion() const noexcept { return motion_.res; }
	ID3D12Resource *confidence() const noexcept { return confidence_.res; }
	ID3D12Resource *global_flow() const noexcept { return global_.res; }
	ID3D12Resource *camera_params() const noexcept { return theta_[0].res; }
	ID3D12Resource *published_camera() const noexcept { return theta_pub_.res; }
	ID3D12Resource *model_weight() const noexcept { return alpha_q_.res; }
	bool camera_model_ran() const noexcept { return model_ran_; }
	static constexpr D3D12_RESOURCE_STATES kPublishedState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	uint32_t width() const noexcept { return width_; }
	uint32_t height() const noexcept { return height_; }
	uint32_t dispatches() const noexcept { return dispatches_; }
	uint32_t records_per_ring() const noexcept;
	uint64_t device_bytes() const noexcept { return device_bytes_; }

	struct ProfileEntry {
		const char *kernel;
		double ms;
	};
	bool set_profiling(bool on, ID3D12CommandQueue *queue);
	bool read_profile(std::vector<ProfileEntry> *out) const;

private:
	struct Tex {
		ID3D12Resource *res = nullptr;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
		uint32_t w = 0, h = 0, mips = 1;
		DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
		uint32_t slot = 0;
	};

	enum Shader : uint32_t {
		kLuma, kDownsample, kCoarseTop, kCoarse, kMedian, kRefine,
		kGlobal, kConfidence, kExport, kCopyFlow,
		kStructure, kModelTerms, kModelReduce, kModelSolve, kDecision, kFuse, kPhotoTerms, kThetaPublish, kShaderCount
	};
	static constexpr uint32_t kFirstModelShader = kStructure;

	struct Constants {
		uint32_t dst_w, dst_h;
		float inv_dst_x, inv_dst_y;
		float inv_src_x, inv_src_y;
		float texel1_x, texel1_y;
		float texel2_x, texel2_y;
		uint32_t mip1, mip2;
		uint32_t first, has_depth;
		float inv_full_x, inv_full_y;
		uint32_t src_mip;
		uint32_t filters;
		uint32_t model_iter;
		uint32_t model_flags;
		float norm_x;
		float px_per_unit;
		uint32_t terms_gx, terms_gy;
		uint32_t partial_rows;
		float dejitter_x, dejitter_y;
		uint32_t colour_space;
		uint32_t pad_colour;
	};

	bool make_pipelines(std::wstring *error);
	bool make_textures(std::wstring *error);
	void make_model_textures();
	void make_flow_views();
	bool make_tex(Tex &t, uint32_t w, uint32_t h, DXGI_FORMAT fmt, uint32_t mips, std::wstring *error);
	void to_state(ID3D12GraphicsCommandList *cmd, Tex &t, D3D12_RESOURCE_STATES want);
	void dispatch(ID3D12GraphicsCommandList *cmd, Shader shader, const Constants &c,
		const D3D12_CPU_DESCRIPTOR_HANDLE *srvs, uint32_t srv_count,
		const D3D12_CPU_DESCRIPTOR_HANDLE *uavs, uint32_t uav_count,
		uint32_t groups_x, uint32_t groups_y);
	D3D12_CPU_DESCRIPTOR_HANDLE staging(uint32_t slot) const;
	D3D12_CPU_DESCRIPTOR_HANDLE srv_of(const Tex &t) const;
	D3D12_CPU_DESCRIPTOR_HANDLE uav_of(const Tex &t) const;

	void level(ID3D12GraphicsCommandList *cmd, Tex &parent, Tex &a, Tex &b,
		uint32_t mip1, uint32_t mip2, uint32_t first);
	void record_camera_model(ID3D12GraphicsCommandList *cmd, const Constants &base);

	ID3D12Device *device_ = nullptr;
	ID3D12RootSignature *root_ = nullptr;
	ID3D12PipelineState *pso_[2][kShaderCount]{};
	ID3D12DescriptorHeap *heap_ = nullptr;
	ID3D12DescriptorHeap *staging_ = nullptr;
	uint32_t stride_ = 0;
	uint32_t ring_ = 0;

	Tex luma_[2];
	Tex l4_, l3_[2], l2_[2], l1_[2], l0_[2], dense_[2], dense_m_;
	Tex prev_flow_, global_, conf_quarter_, prev_conf_;
	Tex motion_, confidence_;
	Tex struct_q_, partials_, sums_, theta_[2], alpha_q_;
	Tex theta_pub_;
	bool model_cold_ = true;
	bool model_ran_ = false;
	bool model_textures_ = false;
	bool depth_logarithmic_ = false;
	uint32_t colour_space_ = 0;
	std::wstring model_textures_error_;
	uint64_t model_textures_failed_at_ = 0;
	uint32_t model_retry_ms_ = 0;
	enum : int { kModelNone, kModelBuilding, kModelReady, kModelFailed };
	std::atomic<int> model_state_{ kModelNone };
	std::atomic<bool> model_cancel_{ false };
	std::thread model_builder_;
	std::wstring model_build_error_;
	void build_model_pipelines();

	ID3D12Device *textures_failed_on_ = nullptr;
	uint32_t textures_failed_w_ = 0, textures_failed_h_ = 0;
	uint64_t textures_failed_at_ = 0;
	std::wstring textures_error_;

	float prev_jitter_x_ = 0.0f, prev_jitter_y_ = 0.0f;
	uint32_t skipped_ = 0;

	uint32_t width_ = 0, height_ = 0;
	uint32_t frames_ = 0;
	uint32_t dispatches_ = 0;
	uint64_t device_bytes_ = 0;
	uint32_t parity_ = 0;
	uint32_t quality_index_ = 1;
	bool have_depth_ = false;

	static constexpr uint32_t kMaxProfile = 128;
	bool prof_on_ = false;
	ID3D12QueryHeap *prof_heap_ = nullptr;
	ID3D12Resource *prof_readback_ = nullptr;
	UINT64 prof_freq_ = 0;
	uint32_t prof_count_ = 0;
	uint8_t prof_shader_[kMaxProfile]{};
	static const char *kernel_name(uint32_t shader) noexcept;
};

}

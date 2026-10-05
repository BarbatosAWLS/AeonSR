#include "aeon_sr/core/input_probes.hpp"

#include "aeon_sr/interop/native_d3d.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

namespace aeon_sr {

void InputProbes::tick_motion(const PassContext &ctx, const Settings &s, const FrameInputs &in, float last_motion_px)
{
	++frame;
	if (!s.mv_probe || !in.have_motion_vectors || ctx.runtime == nullptr)
		return;
	reshade::api::device *const device = ctx.runtime->get_device();
	if (device == nullptr || in.color.handle == 0)
		return;

	const float sw = static_cast<float>(in.color_info.width);
	const float sh = static_cast<float>(in.color_info.height);

	bool completed = false;
	if (ID3D11Device *const dev11 = native_d3d11_device(device)) {
		ID3D11DeviceContext *const ctx11 = native_d3d11_context(ctx.runtime);
		auto *const mv_res = native_res<ID3D11Resource>(in.motion_vectors);
		if (ctx11 != nullptr && mv_res != nullptr)
			completed = mv11_.tick(dev11, ctx11, mv_res, sw, sh, frame, mv);
	} else if (ID3D12Device *const dev12 = native_d3d12_device(device)) {

		constexpr D3D12_RESOURCE_STATES kSrv =
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		ID3D12GraphicsCommandList *const cmd12 = native_d3d12_list(ctx.cmd_list);
		auto *const mv_res = native_res<ID3D12Resource>(in.motion_vectors);
		if (cmd12 != nullptr && mv_res != nullptr)
			completed = mv12_.tick(dev12, cmd12, mv_res, kSrv, sw, sh, frame, mv);
	}

	if (!completed)
		return;

	mv_moving = in.have_global_flow && last_motion_px > kMvProbeZeroPx;

	wchar_t buf[192]{};
	_snwprintf_s(buf, _TRUNCATE, L"MV probe (%hs): %.0f%% non-zero, mean %.2f px, max %.2f px over %u samples%s",
		motion_provider_label(in.motion_provider),
		mv.nonzero_pct, mv.mean_px, mv.max_px, mv.samples, mv_moving ? L", camera moving" : L"");
	diag_info("probe", buf);
	if (mv.nonzero_pct <= 0.0f && mv_moving) {
		diag_warn("probe",
			L"the camera moved and every sampled motion vector was zero - the estimator is "
			L"running but writing nothing");
	}
}

bool InputProbes::sample_flow(const PassContext &ctx, const FrameInputs &in, float &motion_px)
{
	if (!in.have_global_flow || ctx.runtime == nullptr)
		return false;
	ID3D11Device *const dev11 = native_d3d11_device(ctx.runtime->get_device());
	ID3D11DeviceContext *const ctx11 = native_d3d11_context(ctx.runtime);
	auto *const flow = native_res<ID3D11Resource>(in.global_flow);
	if (dev11 == nullptr || ctx11 == nullptr || flow == nullptr || !flow_.ensure(dev11))
		return false;
	return flow_.sample(ctx11, flow,
		static_cast<float>(in.color_info.width), static_cast<float>(in.color_info.height), motion_px);
}

void InputProbes::reset_motion()
{

	mv11_.restart();
	mv12_.restart();
	mv = MvProbeResult{};
	mv_moving = false;
}

void InputProbes::release()
{
	reset_motion();
	flow_.release();
}

}

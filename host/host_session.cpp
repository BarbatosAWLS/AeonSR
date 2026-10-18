#include "host_session.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/com_release.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/ngx/neural_hardware.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace aeon_sr::host {
namespace {

constexpr uint32_t kMaxPlaneExtent = 16384;

void copy_name(wchar_t (&dst)[remote::kNameChars], const wchar_t *src) noexcept
{
	uint32_t i = 0;
	for (; i + 1 < remote::kNameChars; ++i) {
		const wchar_t c = src[i];
		if (c == L'\0')
			break;
		dst[i] = c;
	}
	dst[i] = L'\0';
}

std::wstring hresult_text(const wchar_t *what, HRESULT hr)
{
	wchar_t buf[128]{};
	swprintf(buf, 128, L"%s 0x%08X", what, static_cast<unsigned int>(hr));
	return buf;
}

const char *backend_label(BackendChoice choice) noexcept
{
	switch (choice) {
	case BackendChoice::Dlss: return "DLSS";
	case BackendChoice::Fsr: return "FSR";
	case BackendChoice::Xess: return "XeSS";
	case BackendChoice::Vsr: return "RTX VSR";
	case BackendChoice::None: break;
	}
	return "none";
}

}

void copy_text(wchar_t *dst, uint32_t dst_chars, const std::wstring &text) noexcept
{
	if (dst == nullptr || dst_chars == 0)
		return;
	const size_t room = dst_chars - 1;
	const size_t n = text.size() < room ? text.size() : room;
	for (size_t i = 0; i < n; ++i)
		dst[i] = text[i];
	dst[n] = L'\0';
}

bool HostSession::start(uint32_t game_pid, remote::Block *block, const std::wstring &host_dir)
{
	last_error_.clear();
	if (block == nullptr) {
		last_error_ = L"the engine host was given no shared block to work from.";
		return false;
	}
	block_ = block;
	game_pid_ = game_pid;

	const LUID adapter{ block->adapter_luid_low, block->adapter_luid_high };
	if (!engine_.init_standalone(adapter)) {
		last_error_ = engine_.last_error.empty()
			? std::wstring(L"the engine host could not build a Direct3D 12 device on the "
				L"graphics card the game is rendering on.")
			: engine_.last_error;
		return false;
	}

	const std::wstring data_dir = join_path(host_dir, L"AeonSR_data");
	ngx12_.addon_dir = host_dir;
	ngx12_.data_dir = data_dir;
	dlss_.rt = &ngx12_;
	vsr_.engine = &engine_;
	fsr_.addon_dir = host_dir;
	xess_.addon_dir = host_dir;
	neural_.addon_dir = host_dir;
	neural_.data_dir = data_dir;
	neural_.guide_state = SharedPlane::kState;

	wchar_t line[256]{};
	swprintf(line, 256, L"engine device up for game %u on adapter %08X:%08X, runtimes in %s",
		game_pid_, static_cast<unsigned int>(block->adapter_luid_high),
		static_cast<unsigned int>(block->adapter_luid_low), host_dir.c_str());
	diag_info("host", line);
	return true;
}

HostCapabilities HostSession::capabilities()
{
	HostCapabilities caps;
	if (dlss_.runtime_present())
		caps.backends_present |= 1u << static_cast<uint32_t>(BackendChoice::Dlss);
	if (fsr_.runtime_present())
		caps.backends_present |= 1u << static_cast<uint32_t>(BackendChoice::Fsr);
	if (xess_.runtime_present())
		caps.backends_present |= 1u << static_cast<uint32_t>(BackendChoice::Xess);
	if (engine_.device11() != nullptr)
		caps.backends_present |= 1u << static_cast<uint32_t>(BackendChoice::Vsr);
	caps.neural_present = neural_.ensure_dll_present() ? 1u : 0u;

	GpuInfo gpu;
	if (query_gpu_info(engine_.device(), &gpu))
		caps.gpu_vendor = static_cast<uint32_t>(gpu.vendor);

	wchar_t line[256]{};
	swprintf(line, 256, L"runtimes beside the host: DLSS %s, FSR %s, XeSS %s, neural %s; card %s",
		(caps.backends_present & (1u << static_cast<uint32_t>(BackendChoice::Dlss))) ? L"yes" : L"no",
		(caps.backends_present & (1u << static_cast<uint32_t>(BackendChoice::Fsr))) ? L"yes" : L"no",
		(caps.backends_present & (1u << static_cast<uint32_t>(BackendChoice::Xess))) ? L"yes" : L"no",
		caps.neural_present != 0 ? L"yes" : L"no",
		diag_widen(vendor_label(gpu.vendor)).c_str());
	diag_info("host", line);
	return caps;
}

void HostSession::shutdown()
{
	engine_.wait_cpu(engine_.last_submitted());
	neural_.shutdown_device();
	dlss_.shutdown();
	fsr_.shutdown();
	xess_.shutdown();
	vsr_.shutdown();
	release_planes();
	safe_release(fence_);
	fence_name_[0] = L'\0';
	gpu_timer_.release();
	engine_.shutdown();
	block_ = nullptr;
}

void HostSession::release_plane(Plane &plane) noexcept
{
	if (plane.res != nullptr)
		engine_.wait_cpu(engine_.last_submitted());
	safe_release(plane.res);
	plane.generation = 0;
	plane.name[0] = L'\0';
}

void HostSession::release_planes() noexcept
{
	for (Plane &plane : planes_)
		release_plane(plane);
	release_plane(output_);
}

void HostSession::fail(remote::RemoteStep step, const char *key, std::wstring text,
	const wchar_t *detail)
{
	step_ = step;
	message_ = text;
	if (detail != nullptr && detail[0] != L'\0')
		text += std::wstring(L" (") + detail + L")";
	diag_state(key, DiagLevel::Error, "host", text);
}

ID3D12Resource *HostSession::open_shared_texture(const wchar_t *name, HRESULT *out_hr) const
{
	*out_hr = E_FAIL;
	ID3D12Device *const device = engine_.device();
	if (device == nullptr || name == nullptr || name[0] == L'\0')
		return nullptr;

	HANDLE handle = nullptr;
	HRESULT hr = device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
	if (FAILED(hr) || handle == nullptr) {
		*out_hr = FAILED(hr) ? hr : E_HANDLE;
		return nullptr;
	}
	ID3D12Resource *res = nullptr;
	hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&res));
	CloseHandle(handle);
	if (FAILED(hr) || res == nullptr) {
		safe_release(res);
		*out_hr = FAILED(hr) ? hr : E_HANDLE;
		return nullptr;
	}
	*out_hr = S_OK;
	return res;
}

bool HostSession::ensure_fence()
{
	wchar_t name[remote::kNameChars]{};
	copy_name(name, block_->fence_name);
	if (name[0] == L'\0') {
		fail(remote::RemoteStep::Engine, "host.fence",
			L"the game did not publish the fence the two processes order their work on.");
		return false;
	}
	if (fence_ != nullptr && wcscmp(fence_name_, name) == 0)
		return true;

	safe_release(fence_);
	fence_name_[0] = L'\0';

	ID3D12Device *const device = engine_.device();
	HANDLE handle = nullptr;
	HRESULT hr = device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
	if (FAILED(hr) || handle == nullptr) {
		fail(remote::RemoteStep::Engine, "host.fence",
			L"the engine host could not reach the fence the game published, so it cannot "
			L"order its work against the game's.",
			hresult_text(L"OpenSharedHandleByName", FAILED(hr) ? hr : E_HANDLE).c_str());
		return false;
	}
	hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&fence_));
	CloseHandle(handle);
	if (FAILED(hr) || fence_ == nullptr) {
		safe_release(fence_);
		fail(remote::RemoteStep::Engine, "host.fence",
			L"the engine host could not reach the fence the game published, so it cannot "
			L"order its work against the game's.",
			hresult_text(L"OpenSharedHandle", FAILED(hr) ? hr : E_HANDLE).c_str());
		return false;
	}
	copy_name(fence_name_, name);
	diag_info("host", std::wstring(L"shared fence opened: ") + fence_name_);
	return true;
}

bool HostSession::open_planes()
{
	constexpr uint32_t kCount = static_cast<uint32_t>(remote::PlaneRole::Count);

	for (uint32_t i = 0; i < kCount; ++i)
		if (!open_plane(planes_[i], block_->planes[i],
				diag_widen(remote::plane_role_label(static_cast<remote::PlaneRole>(i)))))
			return false;
	return open_plane(output_, block_->output_plane, L"display-size output");
}

bool HostSession::open_plane(Plane &cached, const remote::PlaneDesc &desc, const std::wstring &role_text)
{
	const uint32_t share = desc.share;
	const uint32_t format = desc.dxgi_format;
	const uint64_t generation = desc.generation;
	wchar_t name[remote::kNameChars]{};
	copy_name(name, desc.name);

	if (share == static_cast<uint32_t>(remote::ShareKind::None) || format == 0 ||
		name[0] == L'\0') {
		release_plane(cached);
		return true;
	}
	if (share == static_cast<uint32_t>(remote::ShareKind::KmtValue)) {
		release_plane(cached);
		fail(remote::RemoteStep::OpenPlanes, "host.planes",
			L"the game offered its " + role_text + L" texture in a form only its own "
			L"process can open. Run the game in a window on the same graphics card, "
			L"or turn the upscaler off.");
		return false;
	}
	if (share != static_cast<uint32_t>(remote::ShareKind::Name)) {
		release_plane(cached);
		fail(remote::RemoteStep::OpenPlanes, "host.planes",
			L"the game offered its " + role_text + L" texture in a way this engine host "
			L"does not know how to open.");
		return false;
	}

	if (cached.res != nullptr && cached.generation == generation &&
		wcscmp(cached.name, name) == 0)
		return true;

	release_plane(cached);
	HRESULT hr = E_FAIL;
	ID3D12Resource *const res = open_shared_texture(name, &hr);
	if (res == nullptr) {
		fail(remote::RemoteStep::OpenPlanes, "host.planes",
			L"the engine host could not open the game's " + role_text + L" texture. "
			L"The game and the host must be on the same graphics card.",
			hresult_text(L"open", hr).c_str());
		return false;
	}
	cached.res = res;
	cached.generation = generation;
	copy_name(cached.name, name);
	diag_info("host", L"opened the " + role_text + L" texture as " + name);
	return true;
}

bool HostSession::shutdown_requested() const noexcept
{
	return block_ != nullptr && block_->shutdown != 0;
}

void HostSession::serve_frame()
{
	step_ = remote::RemoteStep::Ok;
	message_.clear();
	backend_ran_ = static_cast<uint32_t>(BackendChoice::None);
	upscaler_status_ = static_cast<uint32_t>(UpscalerStatus::Idle);
	neural_ran_ = 0;
	out_width_ = 0;
	out_height_ = 0;
	in_width_ = 0;
	in_height_ = 0;

	const uint64_t frame_index = block_->frame_index;
	fence_signal_value_ = block_->fence_signal_value;

	LARGE_INTEGER qpf{}, q0{}, q1{};
	QueryPerformanceFrequency(&qpf);
	QueryPerformanceCounter(&q0);
	const double stall_before = engine_.begin_stall_ms();
	{
		struct Epilogue {
			HostSession *self;
			uint64_t index;
			~Epilogue() { self->finish_frame(index); }
		} epilogue{ this, frame_index };

		run_frame();
	}
	QueryPerformanceCounter(&q1);
	const double serve = qpf.QuadPart != 0
		? 1000.0 * static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(qpf.QuadPart) : 0.0;
	note_stats(serve, engine_.begin_stall_ms() - stall_before);
}

void HostSession::note_stats(double serve_ms, double stall_ms) noexcept
{
	Stats &s = stats_;
	++s.frames;
	s.serve_ms += serve_ms;
	s.serve_max_ms = serve_ms > s.serve_max_ms ? serve_ms : s.serve_max_ms;
	s.stall_ms += stall_ms;
	s.stall_max_ms = stall_ms > s.stall_max_ms ? stall_ms : s.stall_max_ms;
	gpu_timer_.poll();
	if (gpu_timer_.last_ms > 0.0) {
		++s.gpu_frames;
		s.gpu_ms += gpu_timer_.last_ms;
		s.gpu_max_ms = gpu_timer_.last_ms > s.gpu_max_ms ? gpu_timer_.last_ms : s.gpu_max_ms;
	}
	if (ID3D12Fence *const f = engine_.fence()) {
		const uint64_t done = f->GetCompletedValue();
		const uint64_t behind = engine_.last_submitted() > done ? engine_.last_submitted() - done : 0;
		s.behind += behind;
		s.behind_max = behind > s.behind_max ? behind : s.behind_max;
	}
	const ULONGLONG now = GetTickCount64();
	if (s.since == 0)
		s.since = now;
	if (now < s.since + 10000u)
		return;
	diag_logf(DiagLevel::Info, "host", L"host: last 10 s %u frames served in %.2f ms on average (%.1f at most), "
		L"%.2f ms of it waiting for this engine's GPU to free an allocator (%.1f at most); the host's commands took "
		L"%.2f ms on the GPU from first to last (%.1f at most); %.2f of its frames still on its GPU when it answered "
		L"(%llu at most)", s.frames, s.serve_ms / s.frames, s.serve_max_ms, s.stall_ms / s.frames, s.stall_max_ms,
		s.gpu_frames != 0 ? s.gpu_ms / s.gpu_frames : 0.0, s.gpu_max_ms,
		static_cast<double>(s.behind) / s.frames, static_cast<unsigned long long>(s.behind_max));
	s = Stats{};
	s.since = now;
}

void HostSession::take_extras()
{
	char provider[remote::kFsrProviderChars] = {};
	std::memcpy(provider, block_->fsr_provider, sizeof(provider));
	provider[remote::kFsrProviderChars - 1] = '\0';
	fsr_.want_provider = provider;

	const uint32_t restart = block_->neural_restart_epoch;
	const uint32_t accum = block_->accum_epoch;
	const uint32_t target = block_->accum_target;
	const uint32_t capture = block_->capture_epoch;
	const uint32_t frames = block_->capture_frames;
	if (!extras_seen_) {
		extras_seen_ = true;
		restart_epoch_ = restart;
		capture_epoch_ = capture;
		accum_epoch_ = accum;
	}
	if (restart != restart_epoch_) {
		restart_epoch_ = restart;
		neural_.destroy_feature();
		neural_init_failed_ = false;
		diag_info("host.neural", L"the game asked for DLSS neural rendering to start again");
	}
	if (accum != accum_epoch_) {
		accum_epoch_ = accum;
		if (target != 0)
			neural_.accum.start(target);
		else
			neural_.accum.stop();
	}
	if (capture != capture_epoch_) {
		capture_epoch_ = capture;
		wchar_t dir[remote::kPathChars] = {};
		std::memcpy(dir, block_->capture_dir, sizeof(dir));
		dir[remote::kPathChars - 1] = L'\0';
		if (dir[0] != L'\0')
			neural_.request_capture(dir, frames);
	}
}

void HostSession::write_info() noexcept
{
	const NeuralRenderCommon::Text nt = neural_.text();
	uint32_t f = remote::kNeuralInfoValid;
	const auto set = [&](bool on, uint32_t bit) { f |= on ? bit : 0u; };
	set(neural_.dll_present, remote::kNeuralDllPresent);
	set(neural_.shim_present, remote::kNeuralShimPresent);
	set(neural_.initialized, remote::kNeuralInitialized);
	set(neural_.crashed, remote::kNeuralCrashed);
	set(neural_.driver_too_old(), remote::kNeuralDriverTooOld);
	set(neural_.adapter_unsupported(), remote::kNeuralAdapterUnsupported);
	set(neural_.feature_unsupported, remote::kNeuralFeatureUnsupported);
	set(neural_.serves_card(nt), remote::kNeuralServesCard);
	set(neural_.requirement_known, remote::kNeuralRequirementKnown);
	set(nt.targets_known, remote::kNeuralTargetsKnown);
	set(nt.identity.has_certificate, remote::kNeuralCertificate);
	set(neural_init_failed_, remote::kNeuralInitFailed);
	block_->accum_running = neural_.accum.running ? 1u : 0u;
	block_->accum_done = neural_.accum.done;
	block_->accum_target_now = neural_.accum.target;
	block_->accum_holding = neural_.accum.step() == AccumStep::Hold ? 1u : 0u;
	block_->neural_area_share = neural_.last_area_share;
	block_->neural_driver_major = neural_.driver_major;
	block_->neural_driver_minor = neural_.driver_minor;
	block_->neural_required_major = neural_.required_driver_major;
	block_->neural_required_minor = neural_.required_driver_minor;
	block_->neural_min_architecture = neural_.min_architecture;
	block_->neural_card_architecture = neural_.card_architecture;
	block_->neural_runtime_minimum = neural_.runtime_minimum_architecture;
	block_->neural_reported_architecture = neural_.reported_architecture;
	block_->neural_eval_count = neural_.eval_count;
	const auto ascii = [](char *dst, size_t chars, const std::string &text) {
		const size_t n = std::min(text.size(), chars - 1);
		std::memcpy(dst, text.data(), n);
		dst[n] = '\0';
	};
	uint32_t providers = 0;
	if (!fsr_.fsr_loading()) {
		for (const auto &pv : fsr_.providers) {
			if (providers == remote::kFsrProviders)
				break;
			ascii(block_->fsr_providers[providers++], remote::kFsrProviderChars, pv.name);
		}
		copy_text(block_->fsr_provider_version, 64, fsr_.provider_version);
	}
	block_->fsr_provider_count = providers;
	{
		const VsrProcessorStatus &v = vsr_.vsr_status();
		block_->vsr_status = backend_ran_ != static_cast<uint32_t>(BackendChoice::Vsr) ? 0u : (v.queried ? remote::kVsrQueried : 0u) | (v.capable ? remote::kVsrCapable : 0u) |
			(v.enabled ? remote::kVsrEnabled : 0u) | (v.in_use ? remote::kVsrInUse : 0u) | ((v.level & 0xFFu) << 8);
	}
	ascii(block_->neural_runtime_version, sizeof(block_->neural_runtime_version), nt.identity.file_version);
	ascii(block_->neural_runtime_sha256, sizeof(block_->neural_runtime_sha256), nt.sha256);
	ascii(block_->neural_runtime_kernels, sizeof(block_->neural_runtime_kernels),
		nt.targets_known ? neural_kernels_label(nt.kernels) : std::string());
	ascii(block_->neural_runtime_cards, sizeof(block_->neural_runtime_cards),
		nt.targets_known ? neural_kernels_cards(nt.kernels) : std::string());
	copy_text(block_->neural_runtime_file, remote::kPathChars, nt.dll_path);
	copy_text(block_->neural_capture_status, 128, neural_.capture.last);
	MemoryBarrier();
	block_->neural_flags = f;
}

void HostSession::run_frame()
{
	take_extras();
	if (!engine_.ready()) {
		fail(remote::RemoteStep::Engine, "host.engine",
			L"the engine host lost its Direct3D 12 device.");
		return;
	}
	if (!ensure_fence())
		return;
	if (!open_planes())
		return;

	const uint32_t width = block_->width;
	const uint32_t height = block_->height;
	if (width == 0 || height == 0 || width > kMaxPlaneExtent || height > kMaxPlaneExtent) {
		fail(remote::RemoteStep::OpenPlanes, "host.size",
			L"this frame arrived with a size the engine host cannot render at.");
		return;
	}

	ID3D12Resource *const colour = planes_[static_cast<uint32_t>(remote::PlaneRole::Colour)].res;
	if (colour == nullptr) {
		fail(remote::RemoteStep::OpenPlanes, "host.planes",
			L"this frame arrived without a colour texture, so there is nothing to upscale.");
		return;
	}
	ID3D12Resource *const depth = planes_[static_cast<uint32_t>(remote::PlaneRole::Depth)].res;
	ID3D12Resource *const motion = planes_[static_cast<uint32_t>(remote::PlaneRole::Motion)].res;

	ID3D12CommandQueue *const queue = engine_.queue();
	const uint64_t wait_value = block_->fence_wait_value;
	if (wait_value != 0 && FAILED(queue->Wait(fence_, wait_value))) {
		fail(remote::RemoteStep::Engine, "host.engine",
			L"the engine host could not order its work behind the game's.");
		return;
	}

	ID3D12GraphicsCommandList *const cmd = engine_.begin_list();
	const bool timing = cmd != nullptr && gpu_timer_.ensure(engine_.device(), queue);
	if (timing)
		gpu_timer_.begin(cmd);
	if (cmd == nullptr) {
		fail(remote::RemoteStep::Engine, "host.engine",
			engine_.last_error.empty()
				? std::wstring(L"the engine host could not open a command list.")
				: engine_.last_error);
		return;
	}

	FrameInputs inputs;
	inputs.width = width;
	inputs.height = height;
	inputs.have_motion_vectors = block_->have_motion != 0 && motion != nullptr;
	inputs.have_depth = block_->have_depth != 0 && depth != nullptr;
	inputs.motion_provider = inputs.have_motion_vectors
		? MotionProvider::Internal : MotionProvider::None;
	inputs.depth_provider = inputs.have_depth ? DepthProvider::Normalized : DepthProvider::None;

	inputs.engine.device = engine_.device();
	inputs.engine.queue = queue;
	inputs.engine.cmd = cmd;
	inputs.engine.color = colour;
	inputs.engine.depth = inputs.have_depth ? depth : nullptr;
	inputs.engine.motion = inputs.have_motion_vectors ? motion : nullptr;
	inputs.engine.color_state = SharedPlane::kState;
	inputs.engine.depth_state = SharedPlane::kState;
	inputs.engine.motion_state = SharedPlane::kState;
	inputs.engine.native = false;
	if (output_.res != nullptr) {
		inputs.engine.output = output_.res;
		inputs.engine.output_state = SharedPlane::kState;
	}

	vsr_.list_lost = false;
	run_upscaler(inputs);
	if (vsr_.list_lost) {
		fail(remote::RemoteStep::Engine, "host.vsr",
			vsr_.last_error.empty() ? std::wstring(L"the engine host's list could not be reopened after RTX VSR.")
				: vsr_.last_error);
		return;
	}

	run_neural(inputs, output_.res != nullptr ? output_.res : colour, inputs.engine.motion, inputs.engine.depth);

	if (timing)
		gpu_timer_.end(cmd);
	const bool submitted = engine_.submit_list();
	engine_.use_ring(EngineDevice::Ring::Frame);
	if (!submitted) {
		fail(remote::RemoteStep::Engine, "host.submit",
			engine_.last_error.empty()
				? std::wstring(L"the engine host could not submit this frame's work.")
				: engine_.last_error);
	}
}

bool HostSession::run_upscaler(const FrameInputs &inputs)
{
	const uint32_t raw = block_->backend_choice;
	if (raw >= kBackendChoiceCount) {
		fail(remote::RemoteStep::Upscaler, "host.choice",
			L"the game asked for an upscaler this engine host does not have.");
		return false;
	}
	const auto choice = static_cast<BackendChoice>(raw);
	if (choice == BackendChoice::None)
		return true;

	UpscalerParams params = block_->upscaler;
	ID3D12Resource *const guide = block_->have_guide != 0
		? planes_[static_cast<uint32_t>(remote::PlaneRole::Guide)].res : nullptr;
	params.bias_mask = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(guide));

	const auto guide_state = static_cast<D3D12_RESOURCE_STATES>(block_->guide_state);
	const bool wrap_guide = guide != nullptr && choice == BackendChoice::Dlss &&
		guide_state != NgxRuntimeD3D12::kNgxSrvState;
	if (wrap_guide)
		barrier12(inputs.engine.cmd, guide, guide_state, NgxRuntimeD3D12::kNgxSrvState);

	UpscalerBackend *backend = nullptr;
	bool ok = false;
	switch (choice) {
	case BackendChoice::Dlss:
		backend = &dlss_;
		ok = dlss_.run(nullptr, inputs, params);
		break;
	case BackendChoice::Fsr:
		backend = &fsr_;
		ok = fsr_.run(nullptr, inputs, params);
		if (ok) {
			out_width_ = fsr_.out_width();
			out_height_ = fsr_.out_height();
			in_width_ = fsr_.fsr_render_width();
			in_height_ = fsr_.fsr_render_height();
		}
		break;
	case BackendChoice::Vsr:
		backend = &vsr_;
		ok = vsr_.run(nullptr, inputs, params);
		if (ok) {
			out_width_ = vsr_.out_width();
			out_height_ = vsr_.out_height();
			in_width_ = vsr_.vsr_render_width();
			in_height_ = vsr_.vsr_render_height();
		}
		break;
	case BackendChoice::Xess:
		backend = &xess_;
		ok = xess_.run(nullptr, inputs, params);
		if (ok) {
			out_width_ = xess_.out_width();
			out_height_ = xess_.out_height();
			in_width_ = xess_.xess_render_width();
			in_height_ = xess_.xess_render_height();
		}
		break;
	default:
		break;
	}
	if (wrap_guide)
		barrier12(inputs.engine.cmd, guide, NgxRuntimeD3D12::kNgxSrvState, guide_state);
	if (backend == nullptr)
		return false;

	upscaler_status_ = static_cast<uint32_t>(backend->status);
	if (!ok && backend->status == UpscalerStatus::Loading) {
		step_ = remote::RemoteStep::Upscaler;
		message_.clear();
		return false;
	}
	if (!ok) {
		fail(remote::RemoteStep::Upscaler, "host.upscaler",
			backend->last_error.empty()
				? std::wstring(diag_widen(backend_label(choice))) + L" could not run on this frame."
				: backend->last_error);
		return false;
	}
	backend_ran_ = raw;
	if (choice == BackendChoice::Dlss) {
		out_width_ = ngx12_.width;
		out_height_ = ngx12_.height;
		in_width_ = ngx12_.render_width;
		in_height_ = ngx12_.render_height;
	}
	return true;
}

void HostSession::run_neural(const FrameInputs &inputs, ID3D12Resource *colour,
	ID3D12Resource *motion, ID3D12Resource *depth)
{
	if (block_->neural_run == 0)
		return;
	if (neural_.crashed) {
		neural_ran_ = 0xFFFFFFFFu;
		return;
	}

	const bool want_depth = block_->neural_want_depth != 0;
	const NeuralRenderParams np = block_->neural;

	ID3D12Device *const device = engine_.device();
	if (neural_.initialized && neural_.device != device)
		neural_.shutdown_device();

	if (!neural_.initialized) {
		if (neural_init_failed_ || !neural_.init_device(device, inputs.width, inputs.height)) {
			neural_init_failed_ = true;
			fail_neural(neural_.last_error.empty()
				? std::wstring(L"DLSS neural rendering could not start on this machine.") : neural_.last_error);
			return;
		}
	}
	neural_.set_command_queue(engine_.queue());

	float depth_u = 0.0f, depth_v = 0.0f;
	remote::depth_jitter_to_read(*block_, backend_ran_, &depth_u, &depth_v);
	const bool ok = neural_.run(inputs.engine.cmd, colour, SharedPlane::kState,
		motion, want_depth ? depth : nullptr, np, depth_u, depth_v);
	neural_ran_ = ok ? 1u : 0xFFFFFFFFu;
	if (!ok)
		fail_neural(neural_.last_error.empty()
			? std::wstring(L"DLSS neural rendering did not run on this frame.") : neural_.last_error);
}

void HostSession::fail_neural(const std::wstring &text)
{
	neural_ran_ = 0xFFFFFFFFu;
	if (step_ == remote::RemoteStep::Ok)
		message_ = text;
	diag_state("host.neural", DiagLevel::Error, "host", text);
}

void HostSession::finish_frame(uint64_t frame_index)
{
	engine_.abandon_list();

	ID3D12CommandQueue *const queue = engine_.queue();
	if (fence_ != nullptr && queue != nullptr)
		queue->Signal(fence_, fence_signal_value_);

	if (block_ == nullptr)
		return;

	block_->step = static_cast<uint32_t>(step_);
	block_->backend_ran = backend_ran_;
	block_->upscaler_status = upscaler_status_;
	block_->neural_ran = neural_ran_;
	block_->upscaler_out_width = out_width_;
	block_->upscaler_out_height = out_height_;
	block_->upscaler_in_size = (in_width_ & 0xFFFFu) | ((in_height_ & 0xFFFFu) << 16);
	block_->neural_model_width = neural_.model_width;
	block_->neural_model_height = neural_.model_height;
	block_->neural_eval_rows = neural_.last_eval_rows;
	block_->neural_passes_built = neural_.created_passes;
	block_->neural_gpu_us = neural_.timer.last_ms > 0.0
		? static_cast<uint32_t>(neural_.timer.last_ms * 1000.0 + 0.5) : 0u;
	copy_text(block_->message, remote::kTextChars, message_);
	write_info();
	MemoryBarrier();
	block_->response_index = frame_index;
}

}

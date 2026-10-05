#include "aeon_sr/interop/remote_engine.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/gpu12.hpp"
#include "aeon_sr/core/settings.hpp"

#include <atomic>
#include <cstdio>
#include <vector>

namespace aeon_sr {
namespace {

template <typename T>
void safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

void safe_close(HANDLE &h)
{
	if (h != nullptr && h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
	h = nullptr;
}

uint32_t load32(uint32_t &field) noexcept
{
	return std::atomic_ref<uint32_t>(field).load(std::memory_order_acquire);
}

uint64_t load64(uint64_t &field) noexcept
{
	return std::atomic_ref<uint64_t>(field).load(std::memory_order_acquire);
}

void store32(uint32_t &field, uint32_t value) noexcept
{
	std::atomic_ref<uint32_t>(field).store(value, std::memory_order_release);
}

void store64(uint64_t &field, uint64_t value) noexcept
{
	std::atomic_ref<uint64_t>(field).store(value, std::memory_order_release);
}

std::wstring bounded_text(const wchar_t *text, uint32_t chars)
{
	uint32_t n = 0;
	while (n < chars && text[n] != L'\0')
		++n;
	return std::wstring(text, text + n);
}

void log_hr(const wchar_t *what, HRESULT hr)
{
	diag_logf(DiagLevel::Error, "remote", L"%s failed, 0x%08X", what,
		static_cast<unsigned int>(hr));
}

bool path_exists(const std::wstring &path)
{
	return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

const char *role_state_key(uint32_t role) noexcept
{
	switch (static_cast<remote::PlaneRole>(role)) {
	case remote::PlaneRole::Colour: return "remote_plane_colour";
	case remote::PlaneRole::Depth: return "remote_plane_depth";
	case remote::PlaneRole::Motion: return "remote_plane_motion";
	case remote::PlaneRole::Guide: return "remote_plane_guide";
	case remote::PlaneRole::Count: break;
	}
	return "remote_plane";
}

const wchar_t *role_tag(uint32_t role) noexcept
{
	switch (static_cast<remote::PlaneRole>(role)) {
	case remote::PlaneRole::Colour: return L"colour";
	case remote::PlaneRole::Depth: return L"depth";
	case remote::PlaneRole::Motion: return L"motion";
	case remote::PlaneRole::Guide: return L"guide";
	case remote::PlaneRole::Count: break;
	}
	return L"plane";
}

void record_copy(ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *dst, D3D12_RESOURCE_STATES dst_state,
	ID3D12Resource *src, D3D12_RESOURCE_STATES src_state)
{
	D3D12_RESOURCE_BARRIER b[2]{};
	UINT n = 0;
	if (dst_state != D3D12_RESOURCE_STATE_COPY_DEST) {
		b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b[n].Transition.pResource = dst;
		b[n].Transition.StateBefore = dst_state;
		b[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		++n;
	}
	if (src_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
		b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b[n].Transition.pResource = src;
		b[n].Transition.StateBefore = src_state;
		b[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		++n;
	}
	if (n != 0)
		cmd->ResourceBarrier(n, b);
	cmd->CopyResource(dst, src);
	for (UINT i = 0; i < n; ++i) {
		const D3D12_RESOURCE_STATES before = b[i].Transition.StateBefore;
		b[i].Transition.StateBefore = b[i].Transition.StateAfter;
		b[i].Transition.StateAfter = before;
	}
	if (n != 0)
		cmd->ResourceBarrier(n, b);
}

constexpr D3D12_RESOURCE_STATES kSharedState = D3D12_RESOURCE_STATE_COMMON;

std::atomic<uint64_t> g_name_generation{ 1 };

constexpr uint32_t kColour = static_cast<uint32_t>(remote::PlaneRole::Colour);
constexpr uint32_t kDepth = static_cast<uint32_t>(remote::PlaneRole::Depth);
constexpr uint32_t kMotion = static_cast<uint32_t>(remote::PlaneRole::Motion);
constexpr uint32_t kGuide = static_cast<uint32_t>(remote::PlaneRole::Guide);

}

bool RemoteEngine::ready() const noexcept
{
	return block_ != nullptr && request_ != nullptr && response_ != nullptr &&
		host_process_ != nullptr && fence_ != nullptr && handshaked_;
}

std::wstring RemoteEngine::describe() const
{
	if (ready()) {
		wchar_t buf[96];
		swprintf(buf, 96, L"AeonSRHost.exe (64-bit), pid %u", host_pid_);
		return buf;
	}
	if (!last_error.empty())
		return last_error;
	return L"the engine host has not been started";
}

bool RemoteEngine::start(EngineDevice &engine, HMODULE module, reshade::api::device_api game_api)
{
	stop();
	last_error.clear();

	if (!engine.ready() || engine.device() == nullptr) {
		last_error = L"the add-on's own Direct3D 12 device is not up, so there is nothing to "
			L"hand the engine host.";
		return false;
	}
	engine_dev_ = &engine;
	game_pid_ = GetCurrentProcessId();
	remote::session_names(game_pid_, names_);

	map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
		static_cast<DWORD>(sizeof(remote::Block)), names_.block);
	const DWORD map_err = GetLastError();
	if (map_ == nullptr) {
		log_hr(L"CreateFileMapping for the session block", HRESULT_FROM_WIN32(map_err));
		last_error = L"Windows refused the shared memory the engine host talks through.";
		stop();
		return false;
	}
	if (map_err == ERROR_ALREADY_EXISTS) {
		last_error = L"the engine host session is already taken. If the upscaler was just "
			L"switched off, wait a few seconds and turn it on again; if it keeps happening, "
			L"a second copy of Aeon SR is loaded in this game and one of them has to go.";
		stop();
		return false;
	}

	block_ = static_cast<remote::Block *>(
		MapViewOfFile(map_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(remote::Block)));
	if (block_ == nullptr) {
		log_hr(L"MapViewOfFile for the session block", HRESULT_FROM_WIN32(GetLastError()));
		last_error = L"Windows refused the shared memory the engine host talks through.";
		stop();
		return false;
	}

	memset(block_, 0, sizeof(remote::Block));
	block_->magic = remote::kMagic;
	block_->version = remote::kProtocolVersion;
	block_->block_size = remote::kBlockSize;
	block_->game_pid = game_pid_;
	block_->game_api = static_cast<uint32_t>(game_api);
	const LUID luid = engine.luid();
	block_->adapter_luid_low = luid.LowPart;
	block_->adapter_luid_high = luid.HighPart;

	request_ = CreateEventW(nullptr, FALSE, FALSE, names_.request);
	response_ = CreateEventW(nullptr, FALSE, FALSE, names_.response);
	if (request_ == nullptr || response_ == nullptr) {
		log_hr(L"CreateEvent for the session", HRESULT_FROM_WIN32(GetLastError()));
		last_error = L"Windows refused the events the engine host is woken with.";
		stop();
		return false;
	}

	HRESULT hr = engine.device()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_));
	if (FAILED(hr) || fence_ == nullptr) {
		log_hr(L"CreateFence(SHARED)", hr);
		last_error = L"this graphics driver refused a shared fence, which is what keeps the game "
			L"and the engine host in step.";
		stop();
		return false;
	}
	wchar_t fence_name[remote::kNameChars];
	remote::object_name(game_pid_, L"fence", g_name_generation.fetch_add(1),
		fence_name, remote::kNameChars);
	hr = engine.device()->CreateSharedHandle(fence_, nullptr, GENERIC_ALL, fence_name, &fence_handle_);
	if (FAILED(hr) || fence_handle_ == nullptr) {
		log_hr(L"CreateSharedHandle for the fence", hr);
		last_error = L"the engine host session could not be set up. If the upscaler was just "
			L"switched off, the previous host may still be closing: wait a few seconds and turn "
			L"it on again.";
		stop();
		return false;
	}
	memcpy(block_->fence_name, fence_name, sizeof(block_->fence_name));
	fence_value_ = 0;

	const std::wstring dir = module_directory(module);
	host_path_ = join_path(dir, L"AeonSRHost.exe");
	if (!path_exists(host_path_)) {
		last_error = L"AeonSRHost.exe is missing. It ships beside AeonSR.addon and is what runs "
			L"the upscaler for a 32-bit game; copy it into the same folder and restart the game.";
		diag_error("remote", L"no host executable at " + host_path_);
		stop();
		return false;
	}

	std::wstring command = L"\"" + host_path_ + L"\" --pid " + std::to_wstring(game_pid_);
	std::vector<wchar_t> mutable_command(command.begin(), command.end());
	mutable_command.push_back(L'\0');

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(host_path_.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
		log_hr(L"CreateProcess for the host", HRESULT_FROM_WIN32(GetLastError()));
		last_error = L"AeonSRHost.exe would not start. Antivirus software blocking it is the "
			L"usual reason; allow it and restart the game.";
		stop();
		return false;
	}
	host_process_ = pi.hProcess;
	safe_close(pi.hThread);

	const ULONGLONG deadline = GetTickCount64() + remote::kHandshakeTimeoutMs;
	for (;;) {
		if (WaitForSingleObject(host_process_, 0) == WAIT_OBJECT_0) {
			DWORD code = 0;
			GetExitCodeProcess(host_process_, &code);
			wchar_t why[192];
			swprintf(why, 192, L"AeonSRHost.exe started and closed again straight away, with "
				L"exit code %lu. AeonSR.log beside the add-on says what it could not do.", code);
			last_error = why;
			diag_logf(DiagLevel::Error, "remote", L"the host exited during the handshake, code %lu",
				code);
			stop();
			return false;
		}
		if (load32(block_->host_ready) != 0)
			break;
		if (GetTickCount64() >= deadline) {
			last_error = L"AeonSRHost.exe started but never answered. AeonSR.log beside the "
				L"add-on says how far it got.";
			stop();
			return false;
		}
		WaitForSingleObject(host_process_, 1);
	}
	if (WaitForSingleObject(host_process_, 0) == WAIT_OBJECT_0) {
		DWORD code = 0;
		GetExitCodeProcess(host_process_, &code);
		wchar_t why[192];
		swprintf(why, 192, L"AeonSRHost.exe answered and then closed, with exit code %lu. "
			L"AeonSR.log beside the add-on says what it could not do.", code);
		last_error = why;
		diag_logf(DiagLevel::Error, "remote", L"the host exited right after the handshake, code %lu",
			code);
		stop();
		return false;
	}
	handshaked_ = true;

	host_pid_ = load32(block_->host_pid);
	const uint32_t host_major = load32(block_->host_version_major);
	const uint32_t host_minor = load32(block_->host_version_minor);

	{
		unsigned int ours_major = 0, ours_minor = 0;
		if (host_major == 0 && host_minor == 0) {
			diag_warn("remote", L"the engine host did not say which build it is, so it was "
				L"not checked against this add-on's version");
		} else if (sscanf(AEONSR_VERSION_STR, "%u.%u", &ours_major, &ours_minor) == 2) {
			if (host_major != ours_major || host_minor != ours_minor) {
				wchar_t why[224];
				swprintf(why, 224, L"AeonSRHost.exe is version %u.%u and this add-on is %u.%u. "
					L"They ship together and have to match; copy the host from the same "
					L"download as the add-on.", host_major, host_minor, ours_major, ours_minor);
				last_error = why;
				stop();
				return false;
			}
		} else {
			diag_warn("remote", L"this build cannot read its own version, so the host's was "
				L"not checked against it");
		}
	}

	backends_present_ = load32(block_->backends_present);
	neural_present_ = load32(block_->neural_present);
	host_gpu_vendor_ = load32(block_->host_gpu_vendor);
	if (host_gpu_vendor_ > static_cast<uint32_t>(GpuVendor::Intel))
		host_gpu_vendor_ = static_cast<uint32_t>(GpuVendor::Unknown);
	if (host_pid_ == 0)
		host_pid_ = GetProcessId(host_process_);
	first_frame_ = true;
	frame_outstanding_ = false;

	diag_logf(DiagLevel::Info, "remote", L"engine host %u.%u up, pid %u, %s",
		host_major, host_minor, host_pid_, host_path_.c_str());
	diag_logf(DiagLevel::Info, "remote", L"the host found DLSS %s, FSR %s, XeSS %s, "
		L"neural rendering %s, on a %S adapter",
		backend_present(BackendChoice::Dlss) ? L"yes" : L"no",
		backend_present(BackendChoice::Fsr) ? L"yes" : L"no",
		backend_present(BackendChoice::Xess) ? L"yes" : L"no",
		neural_present() ? L"yes" : L"no",
		vendor_label(static_cast<GpuVendor>(host_gpu_vendor_)));
	return true;
}

bool RemoteEngine::backend_present(BackendChoice choice) const noexcept
{
	const uint32_t bit = static_cast<uint32_t>(choice);
	if (bit >= 32u)
		return false;
	return (backends_present_ & (1u << bit)) != 0u;
}

void RemoteEngine::stop()
{
	if (host_process_ != nullptr) {
		if (block_ != nullptr)
			store32(block_->shutdown, 1);
		if (request_ != nullptr)
			SetEvent(request_);
		if (WaitForSingleObject(host_process_, 2000) == WAIT_OBJECT_0) {
			diag_info("remote", L"the engine host exited when asked");
		} else {
			TerminateProcess(host_process_, 0);
			WaitForSingleObject(host_process_, 1000);
			diag_warn("remote", L"the engine host did not exit when asked and was closed");
		}
		safe_close(host_process_);
	}

	if (fence_ != nullptr) {
		if (last_wait_value_ != 0)
			fence_wait(fence_, last_wait_value_, 2000);
		if (fence_value_ != 0 && fence_->GetCompletedValue() < fence_value_)
			fence_->Signal(fence_value_);
	}

	if (engine_dev_ != nullptr && !engine_dev_->drain())
		diag_warn("remote", L"the engine did not drain before the host session was released, "
			L"so its shared planes went while the GPU may still have had them");

	for (uint32_t i = 0; i < kPlanes; ++i)
		release_plane(i);

	safe_release(fence_);
	safe_close(fence_handle_);
	safe_close(request_);
	safe_close(response_);
	if (block_ != nullptr) {
		UnmapViewOfFile(block_);
		block_ = nullptr;
	}
	safe_close(map_);

	engine_dev_ = nullptr;
	host_pid_ = 0;
	backends_present_ = 0;
	neural_present_ = 0;
	host_gpu_vendor_ = static_cast<uint32_t>(GpuVendor::Unknown);
	handshaked_ = false;
	first_frame_ = true;
	fence_value_ = 0;
	frame_index_ = 0;
	last_wait_value_ = 0;
	outstanding_signal_ = 0;
	frame_outstanding_ = false;
}

RemoteEngine::~RemoteEngine()
{
	stop();
}

void RemoteEngine::release_plane(uint32_t role)
{
	SharedTexture &t = planes_[role];
	safe_release(t.res);
	safe_close(t.handle);
	t.width = t.height = 0;
	t.format = DXGI_FORMAT_UNKNOWN;
	t.generation = 0;
	t.name[0] = L'\0';
}

bool RemoteEngine::ensure_plane(EngineDevice &engine, uint32_t role, ID3D12Resource *src,
	remote::PlaneDesc &desc)
{
	memset(&desc, 0, sizeof(desc));
	desc.share = static_cast<uint32_t>(remote::ShareKind::None);
	if (src == nullptr)
		return true;

	const D3D12_RESOURCE_DESC sd = src->GetDesc();
	const uint32_t w = static_cast<uint32_t>(sd.Width);
	const uint32_t h = sd.Height;
	SharedTexture &t = planes_[role];

	if (t.res == nullptr || t.width != w || t.height != h || t.format != sd.Format) {
		engine.drain();
		release_plane(role);

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC d{};
		d.Dimension = sd.Dimension;
		d.Width = sd.Width;
		d.Height = sd.Height;
		d.DepthOrArraySize = sd.DepthOrArraySize;
		d.MipLevels = sd.MipLevels;
		d.Format = sd.Format;
		d.SampleDesc = sd.SampleDesc;
		d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		d.Flags = (role == kColour)
			? (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
			: D3D12_RESOURCE_FLAG_NONE;

		HRESULT hr = engine.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d,
			kSharedState, nullptr, IID_PPV_ARGS(&t.res));
		if (FAILED(hr) || t.res == nullptr) {
			log_hr(L"CreateCommittedResource(HEAP_FLAG_SHARED)", hr);
			release_plane(role);
			return false;
		}

		const uint64_t generation = g_name_generation.fetch_add(1);
		remote::object_name(game_pid_, role_tag(role), generation, t.name, remote::kNameChars);
		hr = engine.device()->CreateSharedHandle(t.res, nullptr, GENERIC_ALL, t.name, &t.handle);
		if (FAILED(hr) || t.handle == nullptr) {
			log_hr(L"CreateSharedHandle for a plane", hr);
			release_plane(role);
			return false;
		}
		t.width = w;
		t.height = h;
		t.format = sd.Format;
		t.generation = generation;
	}

	desc.share = static_cast<uint32_t>(remote::ShareKind::Name);
	desc.dxgi_format = static_cast<uint32_t>(t.format);
	desc.width = t.width;
	desc.height = t.height;
	desc.generation = t.generation;
	memcpy(desc.name, t.name, sizeof(desc.name));
	return true;
}

bool RemoteEngine::host_still_owns_planes() const noexcept
{
	if (!frame_outstanding_ || fence_ == nullptr)
		return false;
	return fence_->GetCompletedValue() < outstanding_signal_;
}

bool RemoteEngine::exchange(EngineDevice &engine, const Request &req,
	const remote::PlaneDesc *descs, Reply &reply)
{
	const uint64_t wait_value = fence_value_ + 1;
	const uint64_t signal_value = fence_value_ + 2;
	fence_value_ = signal_value;
	const HRESULT signalled = engine.queue()->Signal(fence_, wait_value);
	if (FAILED(signalled)) {
		log_hr(L"Signal on the shared fence", signalled);
		reply.step = remote::RemoteStep::Share;
		reply.message = L"the engine device could not tell the host its frame was ready";
		return false;
	}
	last_wait_value_ = wait_value;

	block_->fence_wait_value = wait_value;
	block_->fence_signal_value = signal_value;
	block_->plane_count = kPlanes;
	block_->width = descs[kColour].width;
	block_->height = descs[kColour].height;
	memcpy(block_->planes, descs, sizeof(remote::PlaneDesc) * kPlanes);
	block_->backend_choice = static_cast<uint32_t>(req.backend);
	block_->neural_run = req.run_neural ? 1u : 0u;
	block_->neural_want_depth = req.neural_want_depth ? 1u : 0u;
	block_->have_depth = descs[kDepth].share != 0 ? 1u : 0u;
	block_->have_motion = descs[kMotion].share != 0 ? 1u : 0u;
	block_->have_guide = descs[kGuide].share != 0 ? 1u : 0u;
	block_->guide_state = static_cast<uint32_t>(kSharedState);
	block_->depth_jitter_u = req.depth_jitter_u;
	block_->depth_jitter_v = req.depth_jitter_v;
	block_->upscaler = req.upscaler;
	block_->upscaler.bias_mask = 0;
	block_->neural = req.neural;

	block_->step = static_cast<uint32_t>(remote::RemoteStep::Timeout);
	block_->backend_ran = static_cast<uint32_t>(BackendChoice::None);
	block_->upscaler_status = static_cast<uint32_t>(UpscalerStatus::Idle);
	block_->neural_ran = 0;
	block_->upscaler_out_width = 0;
	block_->upscaler_out_height = 0;
	block_->upscaler_in_size = 0;
	block_->neural_model_width = 0;
	block_->neural_model_height = 0;
	block_->neural_eval_rows = 0;
	block_->neural_passes_built = 0;
	block_->neural_gpu_us = 0;
	block_->message[0] = L'\0';
	store64(block_->response_index, 0);

	const uint64_t frame_index = ++frame_index_;
	store64(block_->frame_index, frame_index);
	frame_outstanding_ = true;
	outstanding_signal_ = signal_value;

	SetEvent(request_);

	const HANDLE waits[2] = { response_, host_process_ };
	const DWORD budget = first_frame_ ? remote::kFirstFrameTimeoutMs : remote::kFrameTimeoutMs;
	const ULONGLONG deadline = GetTickCount64() + budget;
	bool answered = false;
	for (;;) {
		const ULONGLONG now = GetTickCount64();
		const DWORD left = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
		const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, left);
		if (woke == WAIT_OBJECT_0 + 1) {
			last_error = L"AeonSRHost.exe closed while the game was still running. The "
				L"upscaler is off until it is started again.";
			stop();
			reply.step = remote::RemoteStep::Lost;
			reply.message = last_error;
			return false;
		}
		if (woke != WAIT_OBJECT_0)
			break;
		if (load64(block_->response_index) == frame_index) {
			answered = true;
			break;
		}
		diag_trace("remote", L"a late answer to an abandoned frame was thrown away");
	}
	if (!answered)
		first_frame_ = false;
	if (!answered) {
		reply.step = remote::RemoteStep::Timeout;
		reply.message = L"the engine host did not answer in time";
		return false;
	}

	const uint32_t step_value = load32(block_->step);
	reply.step = step_value <= static_cast<uint32_t>(remote::RemoteStep::Share)
		? static_cast<remote::RemoteStep>(step_value) : remote::RemoteStep::Engine;
	const uint32_t ran = load32(block_->backend_ran);
	reply.backend_ran = ran < kBackendChoiceCount
		? static_cast<BackendChoice>(ran) : BackendChoice::None;
	const uint32_t status = load32(block_->upscaler_status);
	reply.upscaler_status = status <= static_cast<uint32_t>(UpscalerStatus::Loading)
		? static_cast<UpscalerStatus>(status) : UpscalerStatus::EvaluateFailed;
	if (reply.upscaler_status != UpscalerStatus::Loading)
		first_frame_ = false;
	const uint32_t neural = load32(block_->neural_ran);
	reply.neural_ran = neural == 0xFFFFFFFFu ? -1 : (neural == 0 ? 0 : 1);
	reply.out_width = load32(block_->upscaler_out_width);
	reply.out_height = load32(block_->upscaler_out_height);
	const uint32_t in_size = load32(block_->upscaler_in_size);
	reply.in_width = in_size & 0xFFFFu;
	reply.in_height = in_size >> 16;
	reply.neural_model_width = load32(block_->neural_model_width);
	reply.neural_model_height = load32(block_->neural_model_height);
	reply.neural_eval_rows = load32(block_->neural_eval_rows);
	reply.neural_passes_built = load32(block_->neural_passes_built);
	reply.neural_gpu_ms = static_cast<float>(load32(block_->neural_gpu_us)) / 1000.0f;
	reply.message = bounded_text(block_->message, remote::kTextChars);

	if (reply.step != remote::RemoteStep::Ok) {
		if (fence_wait(fence_, signal_value, 50))
			frame_outstanding_ = false;
		if (reply.message.empty())
			reply.message = diag_widen(remote::remote_step_label(reply.step));
		return false;
	}

	if (WaitForSingleObject(host_process_, 0) == WAIT_OBJECT_0) {
		if (!fence_wait(fence_, signal_value, 50)) {
			last_error = L"AeonSRHost.exe closed part way through a frame. The upscaler is "
				L"off until it is started again.";
			stop();
			reply.step = remote::RemoteStep::Lost;
			reply.message = last_error;
			return false;
		}
		frame_outstanding_ = false;
		return true;
	}
	const HRESULT ordered = engine.queue()->Wait(fence_, signal_value);
	if (FAILED(ordered)) {
		log_hr(L"Wait on the shared fence", ordered);
		reply.step = remote::RemoteStep::Share;
		reply.message = L"the engine device could not order itself behind the host";
		return false;
	}
	frame_outstanding_ = false;
	return true;
}

RemoteEngine::Row RemoteEngine::upscaler_row(const Reply &reply, const FrameInputs &inputs)
{
	Row row;
	const bool answered = reply.step == remote::RemoteStep::Ok || reply.step == remote::RemoteStep::Upscaler;
	if (answered && reply.upscaler_status == UpscalerStatus::NeedDepth) {
		row.status = UpscalerStatus::NeedDepth;
		row.message = dlss_depth_message(inputs);
	} else if (answered && reply.upscaler_status == UpscalerStatus::Loading) {
		row.status = UpscalerStatus::Loading;
	} else if (reply.step == remote::RemoteStep::Ok) {
		row.status = reply.upscaler_status;
	} else {
		row.status = UpscalerStatus::EvaluateFailed;
		row.message = reply.message.empty() ? diag_widen(remote::remote_step_label(reply.step)) : reply.message;
	}
	return row;
}

bool RemoteEngine::reports_neural(const Reply &reply) noexcept
{
	return reply.step == remote::RemoteStep::Ok ||
		(reply.step == remote::RemoteStep::Upscaler && reply.neural_ran < 0);
}

bool RemoteEngine::run(EngineDevice &engine, const Request &req, Reply &reply)
{
	reply = Reply{};

	if (!ready()) {
		reply.step = remote::RemoteStep::NoHost;
		reply.message = last_error.empty()
			? std::wstring(L"the engine host is not running") : last_error;
		return false;
	}
	store32(block_->shutdown, 0);

	if (req.inputs == nullptr || !engine.ready() || engine.queue() == nullptr) {
		reply.step = remote::RemoteStep::Share;
		reply.message = L"this frame never reached the engine device";
		return false;
	}

	const EngineFrame &ef = req.inputs->engine;
	if (ef.color == nullptr || ef.cmd == nullptr) {
		reply.step = remote::RemoteStep::Share;
		reply.message = L"this frame never reached the engine device";
		return false;
	}
	reshade::api::command_list *const game_list = (engine.borrowed() && req.game_queue != nullptr)
		? req.game_queue->get_immediate_command_list() : nullptr;
	if (engine.borrowed() && (game_list == nullptr ||
			reinterpret_cast<ID3D12GraphicsCommandList *>(game_list->get_native()) != ef.cmd)) {
		reply.step = remote::RemoteStep::Share;
		reply.message = L"this frame is not on ReShade's own command list, so it cannot be "
			L"handed to the engine host";
		return false;
	}

	if (req.backend == BackendChoice::None && !req.run_neural) {
		reply.step = remote::RemoteStep::Ok;
		reply.upscaler_status = UpscalerStatus::Idle;
		reply.message = L"nothing was asked of the engine host this frame";
		return false;
	}

	if (frame_outstanding_) {
		if (host_still_owns_planes()) {
			if (WaitForSingleObject(host_process_, 0) == WAIT_OBJECT_0) {
				last_error = L"AeonSRHost.exe closed while the game was still running. The "
					L"upscaler is off until it is started again.";
				stop();
				reply.step = remote::RemoteStep::Lost;
				reply.message = last_error;
				return false;
			}
			reply.step = remote::RemoteStep::Timeout;
			reply.message = L"the engine host is still busy with an earlier frame";
			return false;
		}
		frame_outstanding_ = false;
	}

	ID3D12Resource *const sources[kPlanes] = { ef.color, ef.depth, ef.motion, req.guide };
	const D3D12_RESOURCE_STATES source_states[kPlanes] = {
		ef.color_state, ef.depth_state, ef.motion_state, req.guide_state,
	};

	remote::PlaneDesc descs[kPlanes]{};
	for (uint32_t i = 0; i < kPlanes; ++i) {
		if (ensure_plane(engine, i, sources[i], descs[i]))
			continue;
		if (i == kColour) {
			reply.step = remote::RemoteStep::Share;
			reply.message = L"this graphics driver would not make the frame shareable with the "
				L"engine host.";
			return false;
		}
		diag_state(role_state_key(i), DiagLevel::Warn, "remote",
			std::wstring(L"the ") + role_tag(i) + L" plane could not be shared with the engine "
			L"host, so this frame goes without it");
		memset(&descs[i], 0, sizeof(descs[i]));
	}

	for (uint32_t i = 0; i < kPlanes; ++i) {
		if (descs[i].share == static_cast<uint32_t>(remote::ShareKind::None))
			continue;
		record_copy(ef.cmd, planes_[i].res, kSharedState, sources[i], source_states[i]);
	}
	bool submitted = false;
	if (engine.borrowed()) {
		const reshade::api::resource colour{ reinterpret_cast<uintptr_t>(planes_[kColour].res) };
		game_list->barrier(colour, reshade::api::resource_usage::unordered_access,
			reshade::api::resource_usage::unordered_access);
		req.game_queue->flush_immediate_command_list();
		submitted = true;
	} else {
		submitted = engine.submit_list();
	}
	if (!submitted) {
		reply.step = remote::RemoteStep::Share;
		reply.message = L"the engine device could not submit this frame";
		reply.list_recording = false;
		return false;
	}

	const bool copy_back = exchange(engine, req, descs, reply);

	ID3D12GraphicsCommandList *const back = engine.borrowed()
		? reinterpret_cast<ID3D12GraphicsCommandList *>(game_list->get_native())
		: engine.begin_list();
	if (engine.borrowed() && back != ef.cmd) {
		diag_error("remote", L"ReShade replaced its command list while submitting the copy to "
			L"the engine host");
		reply.list_recording = false;
		if (reply.step == remote::RemoteStep::Ok) {
			reply.step = remote::RemoteStep::Share;
			reply.message = L"the game's command list was replaced during the host round trip";
		}
		return false;
	}
	if (back == nullptr) {
		diag_error("remote", L"the engine command list could not be reopened after the host "
			L"round trip, so this frame does not reach the game");
		reply.list_recording = false;
		if (reply.step == remote::RemoteStep::Ok) {
			reply.step = remote::RemoteStep::Share;
			reply.message = L"the engine device could not take the finished frame back";
		}
		return false;
	}
	if (!copy_back || planes_[kColour].res == nullptr)
		return false;

	record_copy(back, ef.color, ef.color_state, planes_[kColour].res, kSharedState);
	return true;
}

}

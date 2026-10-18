#include "aeon_sr/framegen/vendor_frame_gen_remote.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/framegen/host_launch.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace aeon_sr {

namespace {

constexpr uint32_t kHandshakeMs = 5000;
constexpr uint32_t kFirstFrameMs = 20000;
constexpr uint32_t kFrameMs = 2000;
constexpr uint32_t kMostStarts = 2;
constexpr uint32_t kForgiveAfter = 3600;

std::wstring error_text(const wchar_t *what, DWORD error)
{
	wchar_t buf[160];
	swprintf(buf, 160, L"%ls (error %lu)", what, static_cast<unsigned long>(error));
	return buf;
}

std::wstring generator_name(FrameGenerator g)
{
	const char *const label = frame_generator_label(g);
	return std::wstring(label, label + std::strlen(label));
}

bool shared_tex(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format, bool written_there,
	ID3D12Resource **out)
{
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = format;
	d.SampleDesc.Count = 1;
	d.Flags = written_there ? (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		: D3D12_RESOURCE_FLAG_NONE;
	return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON,
		nullptr, IID_PPV_ARGS(out)));
}

template <typename T>
void release(T *&p) noexcept
{
	if (p != nullptr)
		p->Release();
	p = nullptr;
}

void close(HANDLE &h) noexcept
{
	if (h != nullptr)
		CloseHandle(h);
	h = nullptr;
}

}

std::wstring vendor_frame_gen_host_path()
{
	wchar_t v[MAX_PATH] = {};
	const DWORD n = GetEnvironmentVariableW(L"AEONSR_FG_VENDOR_HOST", v, MAX_PATH);
	const bool set = n > 0 && n < MAX_PATH;
	if (set && wcscmp(v, L"0") == 0)
		return std::wstring();
	if (set && wcscmp(v, L"1") != 0)
		return std::wstring(v);
#if defined(_WIN64)
	if (!set)
		return std::wstring();
#endif
	return host_beside_addon(nullptr);
}

std::unique_ptr<VendorFrameGenerator> make_vendor_frame_generator(FrameGenerator g)
{
	if (g == FrameGenerator::AeonSR || static_cast<uint32_t>(g) >= kFrameGeneratorCount)
		return nullptr;
	std::wstring host = vendor_frame_gen_host_path();
	if (!host.empty())
		return std::make_unique<RemoteVendorFrameGenerator>(g, std::move(host));
	return make_inprocess_vendor_frame_generator(g);
}

VendorStep vendor_step_from_host(uint32_t step) noexcept
{
	switch (static_cast<fg_host::Step>(step)) {
	case fg_host::Step::Generated: return VendorStep::Generated;
	case fg_host::Step::Warming: return VendorStep::Warming;
	case fg_host::Step::Building: return VendorStep::Building;
	case fg_host::Step::Unavailable: return VendorStep::Unavailable;
	default: return VendorStep::Failed;
	}
}

void RemoteVendorFrameGenerator::Shared::release() noexcept
{
	aeon_sr::release(color);
	aeon_sr::release(motion);
	aeon_sr::release(depth);
	for (ID3D12Resource *&o : outputs)
		aeon_sr::release(o);
	aeon_sr::release(to_host);
	aeon_sr::release(to_game);
	device = nullptr;
	width = height = 0;
	format = DXGI_FORMAT_UNKNOWN;
}

RemoteVendorFrameGenerator::RemoteVendorFrameGenerator(FrameGenerator kind, std::wstring host_exe)
	: kind_(kind), exe_(std::move(host_exe))
{
}

RemoteVendorFrameGenerator::~RemoteVendorFrameGenerator()
{
	if (starter_.joinable())
		starter_.join();
	end_host(false);
	shared_.release();
	for (Retired &r : retired_)
		r.s.release();
	retired_.clear();
}

std::wstring RemoteVendorFrameGenerator::version() const
{
	std::lock_guard<std::mutex> lock(version_lock_);
	return version_;
}

bool RemoteVendorFrameGenerator::make_shared(const VendorFrame &f, std::wstring *why)
{
	Shared s;
	s.device = f.device;
	s.width = f.width;
	s.height = f.height;
	s.format = f.format;
	const D3D12_RESOURCE_DESC md = f.motion->GetDesc();
	const D3D12_RESOURCE_DESC dd = f.depth->GetDesc();
	bool ok = shared_tex(f.device, f.width, f.height, f.format, false, &s.color) &&
		shared_tex(f.device, static_cast<uint32_t>(md.Width), md.Height, md.Format, false, &s.motion) &&
		shared_tex(f.device, static_cast<uint32_t>(dd.Width), dd.Height, dd.Format, false, &s.depth);
	for (uint32_t i = 0; i < fg_host::kMaxOutputs && ok; ++i)
		ok = shared_tex(f.device, f.width, f.height, f.format, true, &s.outputs[i]);
	ok = ok && SUCCEEDED(f.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.to_host))) &&
		SUCCEEDED(f.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.to_game)));
	if (!ok) {
		s.release();
		*why = generator_name(kind_) + L" frame generation: the textures shared with AeonSRHost.exe could not be made";
		return false;
	}
	shared_ = s;
	to_host_value_ = 0;
	to_game_value_ = 0;
	owed_ = 0;
	return true;
}

void RemoteVendorFrameGenerator::start_host()
{
	std::wstring why;
	const auto refuse = [&](const std::wstring &text) {
		start_error_ = text;
		end_host(true);
		starting_ = false;
	};
	mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(fg_host::Block), nullptr);
	if (mapping_ == nullptr)
		return refuse(error_text(L"the block shared with AeonSRHost.exe could not be made", GetLastError()));
	block_ = static_cast<fg_host::Block *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fg_host::Block)));
	request_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	response_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (block_ == nullptr || request_ == nullptr || response_ == nullptr)
		return refuse(L"the block or the events shared with AeonSRHost.exe could not be made");
	std::memset(block_, 0, sizeof(fg_host::Block));

	HostLaunch launch;
	if (!launch_host_suspended(exe_, mapping_, L" --framegen", L"AeonSRHost-framegen.log", &launch, &why))
		return refuse(why);
	process_ = launch.process;

	bool ok = true;
	ID3D12Device *const device = shared_.device;
	const auto give_shared = [&](ID3D12DeviceChild *object) -> uint64_t {
		HANDLE h = nullptr;
		if (object == nullptr || FAILED(device->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &h))) {
			ok = false;
			return 0;
		}
		const uint64_t v = give_handle(process_, h, 0, true, &ok);
		CloseHandle(h);
		return v;
	};
	fg_host::Block &b = *block_;
	b.magic = fg_host::kMagic;
	b.version = fg_host::kVersion;
	b.block_size = fg_host::kBlockSize;
	b.game_pid = GetCurrentProcessId();
	b.generator = static_cast<uint32_t>(kind_);
	b.width = shared_.width;
	b.height = shared_.height;
	b.format = static_cast<uint32_t>(shared_.format);
	const LUID luid = device->GetAdapterLuid();
	b.adapter_luid_low = luid.LowPart;
	b.adapter_luid_high = luid.HighPart;
	b.output_count = fg_host::kMaxOutputs;
	b.color = give_shared(shared_.color);
	b.motion = give_shared(shared_.motion);
	b.depth = give_shared(shared_.depth);
	for (uint32_t i = 0; i < fg_host::kMaxOutputs; ++i)
		b.outputs[i] = give_shared(shared_.outputs[i]);
	b.to_host = give_shared(shared_.to_host);
	b.to_game = give_shared(shared_.to_game);
	b.game_process = give_handle(process_, GetCurrentProcess(), SYNCHRONIZE, false, &ok);
	b.request_event = give_handle(process_, request_, 0, true, &ok);
	b.response_event = give_handle(process_, response_, 0, true, &ok);
	MemoryBarrier();
	if (!ok) {
		CloseHandle(launch.thread);
		return refuse(L"the textures and fences could not be handed to AeonSRHost.exe");
	}
	ResumeThread(launch.thread);
	CloseHandle(launch.thread);

	HANDLE waits[2] = { response_, process_ };
	const DWORD hit = WaitForMultipleObjects(2, waits, FALSE, kHandshakeMs);
	MemoryBarrier();
	if (hit != WAIT_OBJECT_0 || block_->host_state != static_cast<uint32_t>(fg_host::HostState::Ready)) {
		wchar_t text[fg_host::kTextChars + 1] = {};
		if (hit == WAIT_OBJECT_0)
			std::memcpy(text, const_cast<const wchar_t *>(block_->message), sizeof(block_->message));
		DWORD code = STILL_ACTIVE;
		GetExitCodeProcess(process_, &code);
		return refuse(hit == WAIT_TIMEOUT ? std::wstring(L"AeonSRHost.exe did not answer in time")
			: hit == WAIT_OBJECT_0 + 1 ? error_text(L"AeonSRHost.exe closed at once", code)
			: std::wstring(L"AeonSRHost.exe could not run it: ") + text);
	}
	host_pid_ = launch.pid;
	alive_ = true;
	watch_host();
	diag_logf(DiagLevel::Info, "framegen", L"%ls frame generation runs in AeonSRHost.exe (pid %u), %ux%u",
		generator_name(kind_).c_str(), launch.pid, shared_.width, shared_.height);
	starting_ = false;
}

bool RemoteVendorFrameGenerator::call(uint32_t timeout_ms)
{
	if (!alive_)
		return false;
	block_->seq = ++seq_;
	MemoryBarrier();
	SetEvent(request_);
	HANDLE waits[2] = { response_, process_ };
	const DWORD hit = WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
	MemoryBarrier();
	if (hit == WAIT_OBJECT_0 && block_->answered_seq == seq_)
		return true;
	DWORD code = STILL_ACTIVE;
	GetExitCodeProcess(process_, &code);
	failed_ = hit == WAIT_TIMEOUT ? std::wstring(L"AeonSRHost.exe stopped answering")
		: hit == WAIT_OBJECT_0 + 1 ? error_text(L"AeonSRHost.exe closed", code)
		: std::wstring(L"AeonSRHost.exe answered another request");
	diag_logf(DiagLevel::Warn, "framegen", L"%ls frame generation: %ls", generator_name(kind_).c_str(), failed_.c_str());
	end_host(true);
	return false;
}

void RemoteVendorFrameGenerator::settle_owed() noexcept
{
	const uint64_t owed = owed_.load();
	if (shared_.to_game != nullptr && shared_.to_game->GetCompletedValue() < owed)
		shared_.to_game->Signal(owed);
}

void CALLBACK RemoteVendorFrameGenerator::on_host_exit(void *self, BOOLEAN)
{
	static_cast<RemoteVendorFrameGenerator *>(self)->settle_owed();
}

void RemoteVendorFrameGenerator::watch_host() noexcept
{
	if (watch_ == nullptr && process_ != nullptr)
		RegisterWaitForSingleObject(&watch_, process_, &RemoteVendorFrameGenerator::on_host_exit, this, INFINITE,
			WT_EXECUTEONLYONCE);
}

void RemoteVendorFrameGenerator::unwatch_host() noexcept
{
	if (watch_ != nullptr)
		UnregisterWaitEx(watch_, INVALID_HANDLE_VALUE);
	watch_ = nullptr;
}

void RemoteVendorFrameGenerator::end_host(bool kill) noexcept
{
	unwatch_host();
	if (process_ != nullptr) {
		if (!kill && alive_ && block_ != nullptr) {
			block_->command = static_cast<uint32_t>(fg_host::Command::Quit);
			block_->seq = ++seq_;
			MemoryBarrier();
			SetEvent(request_);
		} else if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
			TerminateProcess(process_, 0);
			WaitForSingleObject(process_, 2000);
		}
		close(process_);
	}
	alive_ = false;
	host_pid_ = 0;
	settle_owed();
	if (block_ != nullptr)
		UnmapViewOfFile(block_);
	block_ = nullptr;
	close(mapping_);
	close(request_);
	close(response_);
	answered_once_ = false;
	seq_ = 0;
}

void RemoteVendorFrameGenerator::retire_shared() noexcept
{
	if (shared_.device != nullptr)
		retired_.push_back(Retired{ shared_, 0 });
	shared_ = Shared{};
}

void RemoteVendorFrameGenerator::age_retired() noexcept
{
	for (Retired &r : retired_)
		if (++r.frames > 4)
			r.s.release();
	retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
		[](const Retired &r) { return r.s.device == nullptr; }), retired_.end());
}

VendorStep RemoteVendorFrameGenerator::record(const VendorFrame &f, std::wstring *note)
{
	age_retired();
	const std::wstring name = generator_name(kind_);
	if (starting_) {
		*note = name + L" frame generation: AeonSRHost.exe is starting";
		return VendorStep::Building;
	}
	if (starter_.joinable())
		starter_.join();
	if (exe_.empty()) {
		*note = name + L" frame generation needs AeonSRHost.exe beside the add-on (its runtime is 64-bit only)";
		return VendorStep::Unavailable;
	}
	if (f.queue == nullptr || !f.flush || f.device == nullptr || f.color == nullptr || f.motion == nullptr ||
			f.depth == nullptr || f.output == nullptr) {
		*note = name + L" frame generation cannot run on this route";
		return VendorStep::Unavailable;
	}
	if (!start_error_.empty()) {
		*note = name + L" frame generation: " + start_error_;
		start_error_.clear();
		retire_shared();
		if (++failures_ >= kMostStarts)
			return VendorStep::Unavailable;
		retry_after_ = GetTickCount64() + 1000;
		return VendorStep::Failed;
	}
	const bool shape = shared_.device == f.device && shared_.width == f.width && shared_.height == f.height &&
		shared_.format == f.format;
	if (!alive_ || !shape) {
		if (GetTickCount64() < retry_after_) {
			*note = name + L" frame generation: AeonSRHost.exe is starting again";
			return VendorStep::Building;
		}
		if (alive_)
			end_host(false);
		else if (!failed_.empty()) {
			if (failures_ >= kMostStarts) {
				*note = name + L" frame generation: " + failed_;
				return VendorStep::Unavailable;
			}
		}
		retire_shared();
		std::wstring why;
		if (!make_shared(f, &why)) {
			*note = why;
			return VendorStep::Unavailable;
		}
		failed_.clear();
		pending_reset_ = true;
		max_generated_ = 1;
		starting_ = true;
		starter_ = std::thread([this] { start_host(); });
		*note = name + L" frame generation: AeonSRHost.exe is starting";
		return VendorStep::Building;
	}

	const auto copy_in = [&](ID3D12Resource *dst, ID3D12Resource *src, D3D12_RESOURCE_STATES src_state) {
		barrier12(f.cmd, src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier12(f.cmd, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		f.cmd->CopyResource(dst, src);
		barrier12(f.cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		barrier12(f.cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
	};
	copy_in(shared_.color, f.color, kInputState);
	copy_in(shared_.motion, f.motion, f.motion_state);
	copy_in(shared_.depth, f.depth, kInputState);
	if (!f.flush()) {
		*note = name + L" frame generation: the engine's list could not be submitted";
		return VendorStep::ListLost;
	}
	const uint64_t wait_value = ++to_host_value_;
	if (FAILED(f.queue->Signal(shared_.to_host, wait_value))) {
		*note = name + L" frame generation: the engine's queue could not signal AeonSRHost.exe";
		return VendorStep::Failed;
	}
	const uint64_t signal_value = ++to_game_value_;
	const uint32_t generated = std::clamp(f.generated, 1u, fg_host::kMaxOutputs);
	fg_host::Block &b = *block_;
	b.command = static_cast<uint32_t>(fg_host::Command::Frame);
	b.generated = generated;
	b.wait_value = wait_value;
	b.signal_value = signal_value;
	b.reset = f.reset || pending_reset_ ? 1u : 0u;
	b.color_space = static_cast<uint32_t>(f.color_space);
	b.frame_ms = static_cast<float>(std::clamp(f.frame_ms, 0.0, 10000.0));
	b.window = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(f.window));
	if (!call(answered_once_ ? kFrameMs : kFirstFrameMs)) {
		++failures_;
		answered_run_ = 0;
		retry_after_ = GetTickCount64() + 500;
		*note = name + L" frame generation: " + failed_;
		return failures_ >= kMostStarts ? VendorStep::Unavailable : VendorStep::Failed;
	}
	answered_once_ = true;
	pending_reset_ = false;
	if (++answered_run_ >= kForgiveAfter) {
		answered_run_ = 0;
		failures_ = 0;
	}
	owed_ = signal_value;
	f.queue->Wait(shared_.to_game, signal_value);
	if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0)
		settle_owed();

	const uint32_t step = b.step;
	const uint32_t most = b.max_generated;
	host_busy_ = b.busy != 0;
	max_generated_ = std::clamp(most, 1u, fg_host::kMaxOutputs);
	{
		wchar_t v[fg_host::kVersionChars + 1] = {};
		std::memcpy(v, const_cast<const wchar_t *>(b.vendor_version), sizeof(b.vendor_version));
		std::lock_guard<std::mutex> lock(version_lock_);
		version_ = v;
	}
	wchar_t text[fg_host::kTextChars + 1] = {};
	std::memcpy(text, const_cast<const wchar_t *>(b.message), sizeof(b.message));
	if (text[0] != L'\0')
		*note = text;

	const VendorStep out = vendor_step_from_host(step);
	if (out == VendorStep::Generated) {
		ID3D12Resource *const into[fg_host::kMaxOutputs] = { f.output, f.more_outputs[0], f.more_outputs[1],
			f.more_outputs[2], f.more_outputs[3] };
		for (uint32_t i = 0; i < generated; ++i) {
			if (into[i] == nullptr)
				continue;
			barrier12(f.cmd, into[i], kOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
			barrier12(f.cmd, shared_.outputs[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			f.cmd->CopyResource(into[i], shared_.outputs[i]);
			barrier12(f.cmd, shared_.outputs[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			barrier12(f.cmd, into[i], D3D12_RESOURCE_STATE_COPY_DEST, kOutputState);
		}
	} else if (step == static_cast<uint32_t>(fg_host::Step::HostFailed)) {
		if (text[0] == L'\0')
			*note = name + L" frame generation: AeonSRHost.exe's own list failed";
	}
	return out;
}

}

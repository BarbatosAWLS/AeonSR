#include "framegen_host.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/framegen/fg_host_protocol.hpp"
#include "aeon_sr/framegen/vendor_frame_gen.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/interop/interop.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace aeon_sr::host {

namespace {

using namespace aeon_sr::fg_host;

enum Exit : int {
	ExitOk = 0,
	ExitNoBlock = 30,
	ExitSetup = 31,
	ExitWaitFailed = 32,
};

template <typename T>
void release(T *&p) noexcept
{
	if (p != nullptr)
		p->Release();
	p = nullptr;
}

HANDLE as_handle(uint64_t v) noexcept
{
	return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(v));
}

void close(HANDLE &h) noexcept
{
	if (h != nullptr)
		CloseHandle(h);
	h = nullptr;
}

double qpc_ms() noexcept
{
	static const double inv = [] {
		LARGE_INTEGER f{};
		QueryPerformanceFrequency(&f);
		return 1000.0 / static_cast<double>(f.QuadPart);
	}();
	LARGE_INTEGER t{};
	QueryPerformanceCounter(&t);
	return static_cast<double>(t.QuadPart) * inv;
}

void copy_text(volatile wchar_t *dst, uint32_t chars, const std::wstring &text) noexcept
{
	const size_t n = std::min<size_t>(text.size(), chars - 1);
	for (size_t i = 0; i < n; ++i)
		dst[i] = text[i];
	dst[n] = L'\0';
}

class FramegenHost {
public:
	~FramegenHost() { shutdown(); }
	bool start(Block *block, std::wstring *why);
	int serve();
	void shutdown() noexcept;

private:
	void answer(const Request &r) noexcept;
	void frame(const Request &r) noexcept;
	void note_stats() noexcept;

	Block *block_ = nullptr;
	Setup setup_;
	HANDLE game_ = nullptr, request_ = nullptr, response_ = nullptr;
	EngineDevice engine_;
	ID3D12Resource *color_ = nullptr, *motion_ = nullptr, *depth_ = nullptr;
	ID3D12Resource *outputs_[kMaxOutputs] = {};
	ID3D12Fence *to_host_ = nullptr, *to_game_ = nullptr;
	std::unique_ptr<VendorFrameGenerator> vendor_;
	bool reset_next_ = true;
	uint64_t frames_ = 0, generated_ = 0, failed_ = 0;
	std::vector<double> record_ms_;
	double since_ = 0.0;
	VendorStep last_step_ = VendorStep::Warming;
};

bool FramegenHost::start(Block *block, std::wstring *why)
{
	block_ = block;
	const wchar_t *bad = nullptr;
	if (!read_setup(*block, &setup_, &bad)) {
		*why = bad;
		return false;
	}
	game_ = as_handle(block->game_process);
	request_ = as_handle(block->request_event);
	response_ = as_handle(block->response_event);
	HANDLE given[3 + kMaxOutputs + 2] = { as_handle(block->color), as_handle(block->motion), as_handle(block->depth) };
	for (uint32_t i = 0; i < kMaxOutputs; ++i)
		given[3 + i] = as_handle(block->outputs[i]);
	given[3 + kMaxOutputs] = as_handle(block->to_host);
	given[4 + kMaxOutputs] = as_handle(block->to_game);
	const auto close_given = [&]() {
		for (HANDLE &h : given)
			close(h);
	};
	if (game_ == nullptr || request_ == nullptr || response_ == nullptr) {
		*why = L"the game handed over no events";
		close_given();
		return false;
	}
	LUID luid{};
	luid.LowPart = setup_.adapter_luid_low;
	luid.HighPart = setup_.adapter_luid_high;
	if (!engine_.init_standalone(luid) || !engine_.adapter_matched()) {
		*why = engine_.last_error.empty() ? L"no Direct3D 12 device on the game's graphics card" : engine_.last_error.c_str();
		close_given();
		return false;
	}
	ID3D12Device *const device = engine_.device();
	bool opened = SUCCEEDED(device->OpenSharedHandle(given[0], IID_PPV_ARGS(&color_))) &&
		SUCCEEDED(device->OpenSharedHandle(given[1], IID_PPV_ARGS(&motion_))) &&
		SUCCEEDED(device->OpenSharedHandle(given[2], IID_PPV_ARGS(&depth_)));
	for (uint32_t i = 0; i < kMaxOutputs && opened; ++i)
		opened = SUCCEEDED(device->OpenSharedHandle(given[3 + i], IID_PPV_ARGS(&outputs_[i])));
	opened = opened && SUCCEEDED(device->OpenSharedHandle(given[3 + kMaxOutputs], IID_PPV_ARGS(&to_host_))) &&
		SUCCEEDED(device->OpenSharedHandle(given[4 + kMaxOutputs], IID_PPV_ARGS(&to_game_)));
	close_given();
	if (!opened) {
		*why = L"the game's shared textures and fences could not be opened";
		return false;
	}
	const D3D12_RESOURCE_DESC cd = color_->GetDesc();
	if (cd.Width != setup_.width || cd.Height != setup_.height || static_cast<uint32_t>(cd.Format) != setup_.format) {
		*why = L"the shared frame is not the size or format the block says";
		return false;
	}
	vendor_ = make_inprocess_vendor_frame_generator(static_cast<FrameGenerator>(setup_.generator));
	if (vendor_ == nullptr) {
		*why = L"this host has no such frame generator";
		return false;
	}
	since_ = qpc_ms();
	return true;
}

void FramegenHost::answer(const Request &r) noexcept
{
	block_->answered_seq = r.seq;
	MemoryBarrier();
	SetEvent(response_);
}

void FramegenHost::frame(const Request &r) noexcept
{
	const double t0 = qpc_ms();
	const auto settle = [&]() {
		ID3D12CommandQueue *const q = engine_.queue();
		if (q == nullptr || FAILED(q->Signal(to_game_, r.signal_value)))
			to_game_->Signal(r.signal_value);
	};
	const auto fail = [&](const std::wstring &why) {
		settle();
		block_->step = static_cast<uint32_t>(Step::HostFailed);
		copy_text(block_->message, kTextChars, why);
		++failed_;
		answer(r);
	};
	ID3D12CommandQueue *const queue = engine_.queue();
	if (FAILED(queue->Wait(to_host_, r.wait_value)))
		return fail(L"AeonSRHost.exe's queue could not wait for the game's frame");
	ID3D12GraphicsCommandList *const cmd = engine_.begin_list();
	if (cmd == nullptr)
		return fail(L"AeonSRHost.exe's list could not be opened: " + engine_.last_error);
	const uint32_t count = std::clamp(r.generated, 1u, kMaxOutputs);
	constexpr D3D12_RESOURCE_STATES in = VendorFrameGenerator::kInputState;
	constexpr D3D12_RESOURCE_STATES out = VendorFrameGenerator::kOutputState;
	for (ID3D12Resource *t : { color_, motion_, depth_ })
		barrier12(cmd, t, D3D12_RESOURCE_STATE_COMMON, in);
	for (uint32_t i = 0; i < count; ++i)
		barrier12(cmd, outputs_[i], D3D12_RESOURCE_STATE_COMMON, out);

	bool list_lost = false;
	VendorFrame f;
	f.device = engine_.device();
	f.queue = queue;
	f.cmd = cmd;
	f.flush = [&]() -> bool {
		list_lost = !engine_.submit_list() || engine_.begin_list() != cmd;
		return !list_lost;
	};
	f.color = color_;
	f.motion = motion_;
	f.motion_state = in;
	f.depth = depth_;
	f.output = outputs_[0];
	f.generated = count;
	for (uint32_t i = 1; i < count; ++i)
		f.more_outputs[i - 1] = outputs_[i];
	f.width = setup_.width;
	f.height = setup_.height;
	f.format = static_cast<DXGI_FORMAT>(setup_.format);
	f.color_space = static_cast<VendorColor>(r.color_space);
	f.reset = r.reset || reset_next_;
	f.frame_ms = r.frame_ms;
	f.window = reinterpret_cast<HWND>(static_cast<uintptr_t>(r.window));
	std::wstring note;
	const VendorStep step = vendor_->record(f, &note);
	reset_next_ = false;
	if (list_lost || step == VendorStep::ListLost) {
		engine_.abandon_list();
		return fail(note.empty() ? L"AeonSRHost.exe's list could not be submitted" : note);
	}
	for (ID3D12Resource *t : { color_, motion_, depth_ })
		barrier12(cmd, t, in, D3D12_RESOURCE_STATE_COMMON);
	for (uint32_t i = 0; i < count; ++i)
		barrier12(cmd, outputs_[i], out, D3D12_RESOURCE_STATE_COMMON);
	if (!engine_.submit_list())
		return fail(L"AeonSRHost.exe's list could not be submitted: " + engine_.last_error);
	if (FAILED(queue->Signal(to_game_, r.signal_value)))
		to_game_->Signal(r.signal_value);

	Step s = Step::Failed;
	switch (step) {
	case VendorStep::Generated: s = Step::Generated; ++generated_; break;
	case VendorStep::Warming: s = Step::Warming; break;
	case VendorStep::Building: s = Step::Building; reset_next_ = true; break;
	case VendorStep::Unavailable: s = Step::Unavailable; break;
	default: s = Step::Failed; reset_next_ = true; ++failed_; break;
	}
	if (step != last_step_ && !note.empty())
		diag_logf(DiagLevel::Info, "framegen", L"%ls", note.c_str());
	last_step_ = step;
	block_->step = static_cast<uint32_t>(s);
	block_->max_generated = vendor_->max_generated();
	block_->busy = vendor_->busy() ? 1u : 0u;
	copy_text(block_->vendor_version, kVersionChars, vendor_->version());
	copy_text(block_->message, kTextChars, note);
	++frames_;
	record_ms_.push_back(qpc_ms() - t0);
	answer(r);
}

void FramegenHost::note_stats() noexcept
{
	const double now = qpc_ms();
	if (now - since_ < 10000.0)
		return;
	double p50 = 0.0, pmax = 0.0;
	if (!record_ms_.empty()) {
		std::sort(record_ms_.begin(), record_ms_.end());
		p50 = record_ms_[record_ms_.size() / 2];
		pmax = record_ms_.back();
	}
	diag_logf(DiagLevel::Info, "framegen", L"last %.1f s: %llu frames, %llu generated, %llu failed, each %.2f ms "
		L"(slowest %.2f)", (now - since_) / 1000.0, static_cast<unsigned long long>(frames_),
		static_cast<unsigned long long>(generated_), static_cast<unsigned long long>(failed_), p50, pmax);
	frames_ = generated_ = failed_ = 0;
	record_ms_.clear();
	since_ = now;
}

int FramegenHost::serve()
{
	for (;;) {
		HANDLE waits[2] = { request_, game_ };
		const DWORD hit = WaitForMultipleObjects(2, waits, FALSE, 1000);
		if (hit == WAIT_OBJECT_0 + 1) {
			diag_info("framegen", L"the game exited; the frame generation host is closing");
			return ExitOk;
		}
		if (hit == WAIT_OBJECT_0) {
			MemoryBarrier();
			Request r;
			if (!read_request(*block_, &r)) {
				diag_error("framegen", L"a request this host cannot serve; closing");
				return ExitSetup;
			}
			if (r.command == Command::Quit) {
				answer(r);
				diag_info("framegen", L"the game asked the frame generation host to close");
				return ExitOk;
			}
			if (r.command == Command::Reset) {
				vendor_->reset();
				reset_next_ = true;
				answer(r);
			} else {
				frame(r);
			}
		} else if (hit == WAIT_FAILED) {
			diag_error("framegen", L"the frame generation host lost the handles it serves on");
			return ExitWaitFailed;
		}
		note_stats();
	}
}

void FramegenHost::shutdown() noexcept
{
	if (engine_.ready())
		engine_.wait_cpu(engine_.last_submitted());
	vendor_.reset();
	release(color_);
	release(motion_);
	release(depth_);
	for (ID3D12Resource *&o : outputs_)
		release(o);
	release(to_host_);
	release(to_game_);
	engine_.shutdown();
	close(response_);
	close(request_);
	close(game_);
}

}

int run_framegen_host(uint64_t block_handle)
{
	HANDLE mapping = as_handle(block_handle);
	auto *const block = mapping != nullptr
		? static_cast<Block *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block))) : nullptr;
	if (block == nullptr) {
		diag_error("framegen", L"the game's block could not be mapped");
		if (mapping != nullptr)
			CloseHandle(mapping);
		return ExitNoBlock;
	}
	diag_logf(DiagLevel::Info, "framegen", L"AeonSRHost running frame generation for game %u",
		static_cast<unsigned>(block->game_pid));
	int code = ExitOk;
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
		FramegenHost host;
		std::wstring why;
		if (!host.start(block, &why)) {
			diag_error("framegen", L"frame generation could not run here: " + why);
			if (block->magic != kMagic || block->version != kVersion || block->block_size != kBlockSize) {
				UnmapViewOfFile(block);
				CloseHandle(mapping);
				return ExitSetup;
			}
			copy_text(block->message, kTextChars, why);
			block->host_state = static_cast<uint32_t>(HostState::Failed);
			MemoryBarrier();
			const HANDLE response = as_handle(block->response_event);
			if (response != nullptr)
				SetEvent(response);
			code = ExitSetup;
		} else {
			block->host_pid = GetCurrentProcessId();
			block->host_state = static_cast<uint32_t>(HostState::Ready);
			MemoryBarrier();
			SetEvent(as_handle(block->response_event));
			diag_info("framegen", L"frame generation host ready");
			code = host.serve();
		}
	}
	UnmapViewOfFile(block);
	CloseHandle(mapping);
	return code;
}

}

#include "aeon_sr/framegen/window_host_link.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/framegen/fg_gpu_priority.hpp"
#include "aeon_sr/framegen/host_launch.hpp"

#include <cstring>
#include <initializer_list>
#include <vector>

namespace aeon_sr {

namespace {

constexpr uint32_t kHandshakeMs = 2500;
constexpr uint32_t kCallMs = 500;

std::wstring hr_text(const wchar_t *what, DWORD error)
{
	wchar_t buf[160];
	swprintf(buf, 160, L"%ls (error %lu)", what, static_cast<unsigned long>(error));
	return buf;
}

}

void WindowHostLink::fail(const std::wstring &why) noexcept
{
	alive_ = false;
	if (error_.empty())
		error_ = why;
}

bool WindowHostLink::start(const std::wstring &exe, ID3D12Device *device, ID3D12Resource *const shared[window_host::kShared],
	ID3D12Fence *ready, ID3D12Fence *done, uint32_t w, uint32_t h, DXGI_FORMAT format,
	window_host::ColorSpace color_space, std::wstring *why)
{
	stop(true);
	error_.clear();
	const auto refuse = [&](const std::wstring &text) {
		error_ = text;
		*why = text;
		stop(true);
		return false;
	};
	mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(window_host::Block), nullptr);
	if (mapping_ == nullptr)
		return refuse(hr_text(L"the block shared with AeonSRHost.exe could not be made", GetLastError()));
	block_ = static_cast<window_host::Block *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(window_host::Block)));
	request_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	response_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	released_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (block_ == nullptr || request_ == nullptr || response_ == nullptr || released_ == nullptr)
		return refuse(L"the block or the events shared with AeonSRHost.exe could not be made");
	std::memset(block_, 0, sizeof(window_host::Block));

	HostLaunch launch;
	std::wstring launch_why;
	if (!launch_host_suspended(exe, mapping_, L" --present", L"AeonSRHost-window.log", &launch, &launch_why))
		return refuse(launch_why);
	process_ = launch.process;
	host_pid_ = launch.pid;
	PROCESS_INFORMATION pi{};
	pi.hThread = launch.thread;

	bool ok = true;
	const auto give = [&](HANDLE h, DWORD access, bool same) -> uint64_t {
		return give_handle(process_, h, access, same, &ok);
	};
	const auto give_shared = [&](ID3D12DeviceChild *object) -> uint64_t {
		HANDLE h = nullptr;
		if (object == nullptr || FAILED(device->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &h))) {
			ok = false;
			return 0;
		}
		const uint64_t v = give(h, 0, true);
		CloseHandle(h);
		return v;
	};
	window_host::Block &b = *block_;
	b.magic = window_host::kMagic;
	b.version = window_host::kVersion;
	b.block_size = window_host::kBlockSize;
	b.game_pid = GetCurrentProcessId();
	b.width = w;
	b.height = h;
	b.format = static_cast<uint32_t>(format);
	b.color_space = static_cast<uint32_t>(color_space);
	const LUID luid = device->GetAdapterLuid();
	b.adapter_luid_low = luid.LowPart;
	b.adapter_luid_high = luid.HighPart;
	b.shared_count = window_host::kShared;
	for (uint32_t i = 0; i < window_host::kShared; ++i)
		b.shared[i] = give_shared(shared[i]);
	b.ready_fence = give_shared(ready);
	b.done_fence = give_shared(done);
	b.game_process = give(GetCurrentProcess(), SYNCHRONIZE, false);
	b.request_event = give(request_, 0, true);
	b.response_event = give(response_, 0, true);
	b.released_event = give(released_, 0, true);
	MemoryBarrier();
	if (!ok) {
		CloseHandle(pi.hThread);
		return refuse(L"the textures and fences could not be handed to AeonSRHost.exe");
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);

	HANDLE waits[2] = { response_, process_ };
	const DWORD hit = WaitForMultipleObjects(2, waits, FALSE, kHandshakeMs);
	MemoryBarrier();
	if (hit != WAIT_OBJECT_0 || block_->host_state != static_cast<uint32_t>(window_host::HostState::Ready)) {
		wchar_t text[window_host::kTextChars + 1] = {};
		if (hit == WAIT_OBJECT_0)
			std::memcpy(text, const_cast<const wchar_t *>(block_->message), sizeof(block_->message));
		DWORD code = STILL_ACTIVE;
		GetExitCodeProcess(process_, &code);
		std::wstring reason = hit == WAIT_TIMEOUT ? L"AeonSRHost.exe did not answer in time"
			: hit == WAIT_OBJECT_0 + 1 ? hr_text(L"AeonSRHost.exe closed at once", code)
			: std::wstring(L"AeonSRHost.exe could not show the window: ") + text;
		return refuse(reason);
	}
	alive_ = true;
	GpuClass c = GpuClass::Normal;
	gpu_class_ = get_gpu_class(process_, &c) ? static_cast<int>(c) : -1;
	diag_logf(DiagLevel::Info, "framegen", L"frame generation: its own window is shown by AeonSRHost.exe (pid %u), "
		L"its GPU scheduler class %ls", host_pid_, gpu_class_ >= 0 ? gpu_class_name(c) : L"not known");
	return true;
}

bool WindowHostLink::call(uint32_t timeout_ms)
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
	fail(hit == WAIT_TIMEOUT ? std::wstring(L"AeonSRHost.exe stopped answering")
		: hit == WAIT_OBJECT_0 + 1 ? hr_text(L"AeonSRHost.exe closed", code)
		: std::wstring(L"AeonSRHost.exe answered another request"));
	return false;
}

bool WindowHostLink::present(uint32_t index, uint64_t ready_value, uint64_t done_value, uint32_t sync_interval,
	WindowHostAnswer *out)
{
	if (!alive_)
		return false;
	window_host::Block &b = *block_;
	b.command = static_cast<uint32_t>(window_host::Command::Present);
	b.shared_index = index;
	b.ready_value = ready_value;
	b.done_value = done_value;
	b.sync_interval = sync_interval;
	if (!call(kCallMs))
		return false;
	out->present_hr = static_cast<HRESULT>(b.present_hr);
	out->present_count = b.present_count;
	out->stats_valid = b.stats_valid != 0;
	out->stats_present_count = b.stats_present_count;
	out->stats_sync_qpc = b.stats_sync_qpc;
	return true;
}

bool WindowHostLink::place(bool visible, int x, int y, int cw, int ch)
{
	if (!alive_)
		return false;
	window_host::Block &b = *block_;
	b.command = static_cast<uint32_t>(window_host::Command::Window);
	b.visible = visible ? 1u : 0u;
	b.x = x;
	b.y = y;
	b.cw = cw;
	b.ch = ch;
	return call(kCallMs);
}

void WindowHostLink::stop(bool kill) noexcept
{
	if (process_ != nullptr) {
		if (!kill && alive_ && block_ != nullptr) {
			block_->command = static_cast<uint32_t>(window_host::Command::Quit);
			call(300);
		}
		if (WaitForSingleObject(process_, kill ? 0 : 500) != WAIT_OBJECT_0) {
			TerminateProcess(process_, 0);
			WaitForSingleObject(process_, 2000);
		}
		CloseHandle(process_);
		process_ = nullptr;
	}
	alive_ = false;
	if (block_ != nullptr)
		UnmapViewOfFile(block_);
	block_ = nullptr;
	for (HANDLE *h : { &mapping_, &request_, &response_, &released_ }) {
		if (*h != nullptr)
			CloseHandle(*h);
		*h = nullptr;
	}
	host_pid_ = 0;
	gpu_class_ = -1;
	seq_ = 0;
}

}

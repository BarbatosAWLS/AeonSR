#include "aeon_sr/interop/gpu12.hpp"

#include <cstdio>

namespace aeon_sr {

bool fence_wait(ID3D12Fence *fence, UINT64 target, DWORD timeout_ms) noexcept
{
	if (fence == nullptr)
		return false;
	if (fence->GetCompletedValue() >= target)
		return true;
	const HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (ev == nullptr)
		return false;
	bool ok = false;
	if (SUCCEEDED(fence->SetEventOnCompletion(target, ev)))
		ok = WaitForSingleObject(ev, timeout_ms) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= target;
	CloseHandle(ev);
	return ok;
}

void Gpu12Fence::release() noexcept
{
	if (fence != nullptr) {
		fence->Release();
		fence = nullptr;
	}
	value = 0;
}

bool Gpu12Fence::signal_and_wait(ID3D12Device *device, ID3D12CommandQueue *queue, DWORD timeout_ms) noexcept
{
	if (device == nullptr || queue == nullptr)
		return false;
	if (fence == nullptr) {
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
			return false;
		value = 0;
	}
	const UINT64 v = ++value;
	if (FAILED(queue->Signal(fence, v)))
		return false;
	return fence_wait(fence, v, timeout_ms);
}

bool Gpu12Timer::ensure(ID3D12Device *device, ID3D12CommandQueue *queue) noexcept
{
	if (device == nullptr || queue == nullptr)
		return false;
	if (heap != nullptr && readback != nullptr && frequency != 0)
		return true;
	if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0)
		return false;

	if (heap == nullptr) {
		D3D12_QUERY_HEAP_DESC d{};
		d.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		d.Count = kSlots * 2u;
		if (FAILED(device->CreateQueryHeap(&d, IID_PPV_ARGS(&heap))))
			return false;
	}
	if (readback == nullptr) {
		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC rd{};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width = sizeof(UINT64) * kSlots * 2u;
		rd.Height = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.Format = DXGI_FORMAT_UNKNOWN;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
			return false;
	}
	return true;
}

void Gpu12Timer::begin(ID3D12GraphicsCommandList *cmd) noexcept
{
	if (cmd == nullptr || heap == nullptr || open)
		return;
	cmd->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u);
	open = true;
}

void Gpu12Timer::end(ID3D12GraphicsCommandList *cmd) noexcept
{
	if (cmd == nullptr || heap == nullptr || readback == nullptr || !open)
		return;
	cmd->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u + 1u);
	cmd->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u, 2, readback,
		sizeof(UINT64) * slot * 2u);
	open = false;
	slot = (slot + 1u) % kSlots;
	if (written < kSlots)
		++written;
}

void Gpu12Timer::poll() noexcept
{
	if (readback == nullptr || frequency == 0 || written < kSlots)
		return;
	const uint32_t read_slot = slot;
	UINT64 *stamps = nullptr;
	const D3D12_RANGE range{ sizeof(UINT64) * read_slot * 2u, sizeof(UINT64) * (read_slot * 2u + 2u) };
	if (FAILED(readback->Map(0, &range, reinterpret_cast<void **>(&stamps))) || stamps == nullptr)
		return;
	const UINT64 a = stamps[read_slot * 2u];
	const UINT64 b = stamps[read_slot * 2u + 1u];
	const D3D12_RANGE none{ 0, 0 };
	readback->Unmap(0, &none);
	if (b <= a)
		return;
	const double ms = 1000.0 * static_cast<double>(b - a) / static_cast<double>(frequency);
	if (!(ms > 0.0) || ms > 250.0)
		return;
	samples[sample_next] = ms;
	sample_next = (sample_next + 1u) % kWindow;
	if (sample_count < kWindow)
		++sample_count;
	last_ms = median_ms();
}

double Gpu12Timer::median_ms() const noexcept
{
	if (sample_count == 0)
		return 0.0;
	double sorted[kWindow];
	for (uint32_t i = 0; i < sample_count; ++i)
		sorted[i] = samples[i];
	for (uint32_t i = 1; i < sample_count; ++i) {
		const double v = sorted[i];
		uint32_t j = i;
		while (j > 0 && sorted[j - 1] > v) {
			sorted[j] = sorted[j - 1];
			--j;
		}
		sorted[j] = v;
	}
	return sorted[sample_count / 2];
}

void Gpu12Timer::release() noexcept
{
	if (heap != nullptr) { heap->Release(); heap = nullptr; }
	if (readback != nullptr) { readback->Release(); readback = nullptr; }
	frequency = 0;
	slot = 0;
	written = 0;
	open = false;
	sample_count = 0;
	sample_next = 0;
	last_ms = 0.0;
}

void Gpu12InitList::release() noexcept
{
	if (list != nullptr) {
		list->Release();
		list = nullptr;
	}
	if (alloc != nullptr) {
		alloc->Release();
		alloc = nullptr;
	}
}

ID3D12GraphicsCommandList *Gpu12InitList::begin(ID3D12Device *device) noexcept
{
	if (device == nullptr)
		return nullptr;
	if (alloc == nullptr)
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	if (list == nullptr && alloc != nullptr) {
		if (SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))))
			list->Close();
	}
	if (alloc == nullptr || list == nullptr)
		return nullptr;
	if (FAILED(alloc->Reset()) || FAILED(list->Reset(alloc, nullptr)))
		return nullptr;
	return list;
}

void Gpu12InitList::end(ID3D12Device *device, ID3D12CommandQueue *queue, Gpu12Fence &fence, bool execute) noexcept
{
	if (list == nullptr)
		return;
	list->Close();
	if (!execute || queue == nullptr)
		return;
	ID3D12CommandList *lists[] = { list };
	queue->ExecuteCommandLists(1, lists);
	fence.signal_and_wait(device, queue);
}

const char *StageTimings::label(Stage s) noexcept
{
	switch (s) {
	case Stage::Total: return "total";
	case Stage::Edges: return "edges";
	case Stage::Observe: return "observe";
	case Stage::Interface: return "interface";
	case Stage::Depth: return "depth";
	case Stage::Flow: return "motion";
	case Stage::Publish: return "motion to game";
	case Stage::Mask: return "dlss mask";
	case Stage::Upscaler: return "upscaler";
	case Stage::Neural: return "neural";
	case Stage::Restore: return "interface restore";
	case Stage::Count: break;
	}
	return "?";
}

void StageTimings::begin(Stage s, ID3D12Device *device, ID3D12CommandQueue *queue,
	ID3D12GraphicsCommandList *cmd) noexcept
{
	const uint32_t i = static_cast<uint32_t>(s);
	if (!enabled || suspended_ || i >= kCount || cmd == nullptr || !timers_[i].ensure(device, queue))
		return;
	Gpu12Timer &t = timers_[i];
	const uint32_t before = t.sample_next;
	const uint32_t count = t.sample_count;
	t.poll();
	if (t.sample_next != before || t.sample_count != count)
		add_sample(s, t.samples[(t.sample_next + Gpu12Timer::kWindow - 1u) % Gpu12Timer::kWindow]);
	t.begin(cmd);
}

void StageTimings::end(Stage s, ID3D12GraphicsCommandList *cmd) noexcept
{
	const uint32_t i = static_cast<uint32_t>(s);
	if (!enabled || suspended_ || i >= kCount)
		return;
	timers_[i].end(cmd);
}

void StageTimings::cancel(Stage s) noexcept
{
	const uint32_t i = static_cast<uint32_t>(s);
	if (i < kCount)
		timers_[i].open = false;
}

void StageTimings::abandon_frame() noexcept
{
	for (Gpu12Timer &t : timers_)
		t.open = false;
	suspended_ = true;
}

void StageTimings::add_sample(Stage s, double ms) noexcept
{
	const uint32_t i = static_cast<uint32_t>(s);
	if (i >= kCount || !(ms >= 0.0))
		return;
	sum_[i] += ms;
	++n_[i];
}

bool StageTimings::take_report(uint64_t now_ms, uint64_t period_ms, std::wstring *line)
{
	if (period_start_ms_ == 0) {
		period_start_ms_ = now_ms;
		return false;
	}
	if (now_ms - period_start_ms_ < period_ms)
		return false;
	period_start_ms_ = now_ms;
	std::wstring out;
	for (uint32_t i = 0; i < kCount; ++i) {
		if (n_[i] == 0)
			continue;
		wchar_t buf[96]{};
		_snwprintf_s(buf, _TRUNCATE, L"%s%hs %.3f ms (%u)", out.empty() ? L"" : L", ",
			label(static_cast<Stage>(i)), sum_[i] / static_cast<double>(n_[i]), n_[i]);
		out += buf;
		sum_[i] = 0.0;
		n_[i] = 0;
	}
	if (remote_frames_ != 0) {
		wchar_t buf[160]{};
		_snwprintf_s(buf, _TRUNCATE, L"%s%u frame(s) ran the upscaler in AeonSRHost.exe, and their total and "
			L"upscaler, which wait on the host, are left out", out.empty() ? L"" : L"; ", remote_frames_);
		out += buf;
		remote_frames_ = 0;
	}
	if (out.empty())
		return false;
	if (line != nullptr)
		*line = std::move(out);
	return true;
}

void StageTimings::release() noexcept
{
	for (Gpu12Timer &t : timers_)
		t.release();
	for (uint32_t i = 0; i < kCount; ++i) {
		sum_[i] = 0.0;
		n_[i] = 0;
	}
	period_start_ms_ = 0;
	remote_frames_ = 0;
	suspended_ = false;
}

}

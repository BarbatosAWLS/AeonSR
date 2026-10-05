#include "aeon_sr/upscalers/upscaler_capture.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"

#include <Windows.h>

#include <chrono>
#include <cstring>

namespace aeon_sr {
namespace {

void make_directories(const std::wstring &path)
{
	for (size_t i = 0; i < path.size(); ++i) {
		if ((path[i] == L'\\' || path[i] == L'/') && i > 0 && path[i - 1] != L':' &&
			path[i - 1] != L'\\' && path[i - 1] != L'/')
			CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
	}
	CreateDirectoryW(path.c_str(), nullptr);
}

bool write_file(const std::wstring &path, const void *data, size_t bytes)
{
	const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	const auto *p = static_cast<const unsigned char *>(data);
	bool ok = true;
	while (ok && bytes > 0) {
		const DWORD chunk = bytes > (1u << 30) ? (1u << 30) : static_cast<DWORD>(bytes);
		DWORD put = 0;
		ok = WriteFile(f, p, chunk, &put, nullptr) != 0 && put == chunk;
		p += chunk;
		bytes -= chunk;
	}
	CloseHandle(f);
	return ok;
}

std::wstring widen(const std::string &s)
{
	return std::wstring(s.begin(), s.end());
}

}

UpscalerCaptureD3D12::~UpscalerCaptureD3D12()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = true;
	}
	wake_.notify_all();
	if (writer_.joinable())
		writer_.join();
	for (Frame &f : in_flight_)
		release(f);
	for (Frame &f : to_write_)
		release(f);
	if (drawn_.readback != nullptr)
		drawn_.readback->Release();
	release(scope_cur_);
	for (ID3D12Resource *b : scope_pool_)
		b->Release();
	if (fence_ != nullptr)
		fence_->Release();
}

void UpscalerCaptureD3D12::request(const std::wstring &root, uint32_t frames)
{
	if (left_ > 0 || frames == 0)
		return;
	root_ = root;
	wanted_ = left_ = frames;
	next_index_ = 0;
	last_recorded_ = 0;
	recorded_this_frame_ = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		written_ = 0;
		write_error_.clear();
		status_.clear();
	}
	if (!writer_.joinable())
		writer_ = std::thread([this] { writer_main(); });
}

void UpscalerCaptureD3D12::stop(const std::wstring &why)
{
	left_ = 0;
	set_status(why);
}

void UpscalerCaptureD3D12::set_status(const std::wstring &s)
{
	std::lock_guard<std::mutex> lock(mutex_);
	status_ = s;
}

void UpscalerCaptureD3D12::begin_frame(ID3D12Device *device, ID3D12CommandQueue *queue)
{
	if (left_ > 0 && next_index_ > 0 && !recorded_this_frame_) {
		wchar_t buf[160]{};
		_snwprintf_s(buf, _TRUNCATE, L"stopped after %u frames: a frame went by without the upscaler", next_index_);
		stop(buf);
	}
	recorded_this_frame_ = false;
	++sequence_;
	if (drawn_.readback != nullptr) {
		Frame orphan;
		orphan.info.index = UINT32_MAX;
		orphan.planes[3] = drawn_;
		drawn_ = Plane{};
		{
			std::lock_guard<std::mutex> lock(mutex_);
			pending_bytes_ += orphan.planes[3].total;
		}
		in_flight_.push_back(std::move(orphan));
	}
	collect(device, queue);
}

void UpscalerCaptureD3D12::collect(ID3D12Device *device, ID3D12CommandQueue *queue)
{
	if (in_flight_.empty() || device == nullptr || queue == nullptr)
		return;
	if (fence_ == nullptr && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
		return;
	bool unarmed = false;
	for (const Frame &f : in_flight_)
		unarmed = unarmed || !f.armed;
	if (unarmed && SUCCEEDED(queue->Signal(fence_, ++fence_value_))) {
		for (Frame &f : in_flight_) {
			if (!f.armed) {
				f.fence_value = fence_value_;
				f.armed = true;
			}
		}
	}
	const UINT64 done = fence_->GetCompletedValue();
	bool handed = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (size_t i = 0; i < in_flight_.size();) {
			if (in_flight_[i].armed && in_flight_[i].fence_value <= done) {
				to_write_.push_back(std::move(in_flight_[i]));
				in_flight_.erase(in_flight_.begin() + static_cast<ptrdiff_t>(i));
				handed = true;
			} else {
				++i;
			}
		}
	}
	if (handed)
		wake_.notify_all();
}

bool UpscalerCaptureD3D12::copy_plane(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *res,
	D3D12_RESOURCE_STATES state, const char *file, Plane *plane, CapturedPlane *out)
{
	const D3D12_RESOURCE_DESC desc = res->GetDesc();
	if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1)
		return false;
	device->GetCopyableFootprints(&desc, 0, 1, 0, &plane->footprint, &plane->rows, &plane->row_bytes, &plane->total);
	if (plane->rows == 0 || plane->row_bytes == 0 || plane->total == 0)
		return false;

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC bd{};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = plane->total;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr, IID_PPV_ARGS(&plane->readback))))
		return false;

	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = plane->readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = plane->footprint;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = res;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	const bool transition = state != D3D12_RESOURCE_STATE_COMMON;
	if (transition)
		barrier12(cmd, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	if (transition)
		barrier12(cmd, res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);

	out->file = file;
	out->width = static_cast<uint32_t>(desc.Width);
	out->height = desc.Height;
	out->dxgi_format = static_cast<uint32_t>(desc.Format);
	out->row_bytes = static_cast<uint32_t>(plane->row_bytes);
	return true;
}

void UpscalerCaptureD3D12::record_drawn(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
	D3D12_RESOURCE_STATES state)
{
	if (left_ == 0 || device == nullptr || cmd == nullptr || color == nullptr || drawn_.readback != nullptr)
		return;
	if (!copy_plane(device, cmd, color, state, "drawn.bin", &drawn_, &drawn_info_))
		drawn_ = Plane{};
}

void UpscalerCaptureD3D12::record(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
	ID3D12Resource *mvec, D3D12_RESOURCE_STATES mvec_state,
	ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state,
	CapturedFrameInfo info)
{
	if (left_ == 0 || device == nullptr || cmd == nullptr || color == nullptr || mvec == nullptr)
		return;

	Frame f;
	f.info = std::move(info);
	f.info.index = next_index_;
	f.info.sequence = sequence_;
	f.info.depth = CapturedPlane{};
	f.dir = captured_frame_dir(root_, next_index_);
	bool ok = copy_plane(device, cmd, color, color_state, "color.bin", &f.planes[0], &f.info.color) &&
		copy_plane(device, cmd, mvec, mvec_state, "mvec.bin", &f.planes[1], &f.info.mvec);
	if (ok && depth != nullptr)
		ok = copy_plane(device, cmd, depth, depth_state, "depth.bin", &f.planes[2], &f.info.depth);
	f.info.drawn = CapturedPlane{};
	if (drawn_.readback != nullptr) {
		f.planes[3] = drawn_;
		f.info.drawn = drawn_info_;
		drawn_ = Plane{};
	} else {
		f.info.split_shift_x = f.info.split_shift_y = 0.0f;
		f.info.split_u = f.info.split_v = 0.0f;
	}
	if (!ok) {
		release(f);
		stop(L"stopped after " + std::to_wstring(next_index_) + L" frames: a plane could not be copied");
		return;
	}
	const uint64_t bytes = f.planes[0].total + f.planes[1].total + f.planes[2].total + f.planes[3].total;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (pending_bytes_ + bytes > kMaxPendingBytes) {
			f.info.index = UINT32_MAX;
			in_flight_.push_back(std::move(f));
			pending_bytes_ += bytes;
			left_ = 0;
			status_ = L"stopped after " + std::to_wstring(next_index_) + L" frames: the disk could not keep up";
			recorded_this_frame_ = true;
			return;
		}
		pending_bytes_ += bytes;
	}
	in_flight_.push_back(std::move(f));
	recorded_this_frame_ = true;
	last_recorded_ = sequence_;
	++next_index_;
	--left_;
}

void UpscalerCaptureD3D12::finish(ID3D12Device *device, ID3D12CommandQueue *queue)
{
	scope_close_all();
	if (!in_flight_.empty() && device != nullptr && queue != nullptr) {
		if (fence_ == nullptr)
			device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
		if (fence_ != nullptr && SUCCEEDED(queue->Signal(fence_, ++fence_value_))) {
			const HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (ev != nullptr && fence_->GetCompletedValue() < fence_value_ &&
				SUCCEEDED(fence_->SetEventOnCompletion(fence_value_, ev)))
				WaitForSingleObject(ev, 5000);
			if (ev != nullptr)
				CloseHandle(ev);
			if (fence_->GetCompletedValue() >= fence_value_) {
				std::lock_guard<std::mutex> lock(mutex_);
				for (Frame &f : in_flight_)
					to_write_.push_back(std::move(f));
				in_flight_.clear();
			}
		}
		wake_.notify_all();
	}
	left_ = 0;
	{
		bool stuck = false;
		for (const Frame &f : in_flight_)
			stuck = stuck || (f.scope && f.n != UINT32_MAX);
		wake_.notify_all();
		std::unique_lock<std::mutex> lock(mutex_);
		wake_.wait_for(lock, std::chrono::seconds(20), [this, stuck] {
			return to_write_.empty() && !writer_busy_ && (stuck || !scope_closed_ || scope_done_);
		});
	}
	std::vector<Frame> dropped;
	uint32_t dropped_frames = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto drop = [&](Frame &f) {
			if (f.scope) {
				if (f.n != UINT32_MAX) {
					--scope_outstanding_;
					++dropped_frames;
				}
			} else {
				pending_bytes_ -= f.planes[0].total + f.planes[1].total + f.planes[2].total + f.planes[3].total;
			}
			dropped.push_back(std::move(f));
		};
		for (Frame &f : to_write_)
			drop(f);
		to_write_.clear();
		for (Frame &f : in_flight_)
			drop(f);
		in_flight_.clear();
		if (dropped_frames != 0) {
			const std::wstring note = std::to_wstring(dropped_frames) +
				L" frames not written: the game or its device went away";
			scope_note_ = scope_note_.empty() ? note : scope_note_ + L", " + note;
		}
	}
	wake_.notify_all();
	{
		std::unique_lock<std::mutex> lock(mutex_);
		wake_.wait(lock, [this] { return !writer_busy_ && !scope_finalize_due(); });
	}
	for (Frame &f : dropped)
		release(f);
	if (drawn_.readback != nullptr) {
		drawn_.readback->Release();
		drawn_ = Plane{};
	}
	if (fence_ != nullptr) {
		fence_->Release();
		fence_ = nullptr;
		fence_value_ = 0;
	}
}

size_t UpscalerCaptureD3D12::gpu_objects() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	size_t n = scope_pool_.size() + (fence_ != nullptr ? 1u : 0u) + (drawn_.readback != nullptr ? 1u : 0u) +
		(scope_cur_.scope_buffer != nullptr ? 1u : 0u);
	for (const Frame &f : in_flight_)
		n += f.scope_buffer != nullptr ? 1u : 0u;
	for (const Frame &f : to_write_)
		n += f.scope_buffer != nullptr ? 1u : 0u;
	return n;
}

void UpscalerCaptureD3D12::release(Frame &f) noexcept
{
	for (Plane &p : f.planes) {
		if (p.readback != nullptr) {
			p.readback->Release();
			p.readback = nullptr;
		}
	}
	if (f.scope_buffer != nullptr) {
		f.scope_buffer->Release();
		f.scope_buffer = nullptr;
	}
}

void UpscalerCaptureD3D12::writer_main()
{
	SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
	std::vector<uint8_t> rows;
	for (;;) {
		Frame f;
		bool finalize = false;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return quit_ || !to_write_.empty() || scope_finalize_due(); });
			if (to_write_.empty()) {
				if (!scope_finalize_due())
					return;
				finalize = true;
			} else {
				f = std::move(to_write_.front());
				to_write_.pop_front();
			}
			writer_busy_ = true;
		}
		if (finalize) {
			scope_finalize();
			{
				std::lock_guard<std::mutex> lock(mutex_);
				writer_busy_ = false;
			}
			wake_.notify_all();
			continue;
		}
		if (f.scope) {
			std::wstring error;
			uint32_t members = UINT32_MAX, held = UINT32_MAX;
			uint32_t planes = 0;
			if (f.n != UINT32_MAX && f.scope_buffer != nullptr) {
				make_directories(f.dir);
				void *mapped = nullptr;
				const D3D12_RANGE read{ 0, static_cast<SIZE_T>(f.scope_buffer->GetDesc().Width) };
				if (FAILED(f.scope_buffer->Map(0, &read, &mapped)) || mapped == nullptr) {
					error = L"could not read a plane back";
				} else {
					for (uint32_t i = 0; i < kScopeStagePlanes && error.empty(); ++i) {
						if ((f.scope_planes & (1u << i)) == 0)
							continue;
						const ScopeSlot &s = f.slots[i];
						rows.resize(static_cast<size_t>(s.row_bytes) * s.rows);
						const auto *src = static_cast<const uint8_t *>(mapped) + s.footprint.Offset;
						for (UINT r = 0; r < s.rows; ++r)
							std::memcpy(rows.data() + static_cast<size_t>(r) * static_cast<size_t>(s.row_bytes),
								src + static_cast<size_t>(r) * s.footprint.Footprint.RowPitch,
								static_cast<size_t>(s.row_bytes));
						if (static_cast<ScopePlane>(i) == ScopePlane::Mask)
							count_scope_mask(rows.data(), rows.size(), &members, &held);
						if (!write_file(scope_plane_path(f.dir, f.n, static_cast<ScopePlane>(i)), rows.data(),
								rows.size()))
							error = L"could not write " + f.dir;
						else
							planes |= 1u << i;
					}
					const D3D12_RANGE none{ 0, 0 };
					f.scope_buffer->Unmap(0, &none);
				}
			}
			release(f);
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (f.n != UINT32_MAX) {
					--scope_outstanding_;
					++scope_frames_written_;
					if (f.n < scope_members_.size()) {
						scope_members_[f.n] = members;
						scope_held_[f.n] = held;
					}
					for (uint32_t i = 0; i < kScopeStagePlanes; ++i) {
						if ((planes & (1u << i)) != 0)
							scope_fmt_[i].file = scope_plane_name(static_cast<ScopePlane>(i));
					}
					if (!error.empty() && scope_error_.empty())
						scope_error_ = error;
				}
				writer_busy_ = false;
			}
			wake_.notify_all();
			continue;
		}
		const uint64_t bytes = f.planes[0].total + f.planes[1].total + f.planes[2].total + f.planes[3].total;
		std::wstring error;
		if (f.info.index != UINT32_MAX) {
			make_directories(f.dir);
			const CapturedPlane *files[4] = { &f.info.color, &f.info.mvec, &f.info.depth, &f.info.drawn };
			for (int i = 0; i < 4 && error.empty(); ++i) {
				const Plane &p = f.planes[i];
				if (p.readback == nullptr)
					continue;
				void *mapped = nullptr;
				const D3D12_RANGE read{ 0, static_cast<SIZE_T>(p.total) };
				if (FAILED(p.readback->Map(0, &read, &mapped)) || mapped == nullptr) {
					error = L"could not read a plane back";
					break;
				}
				rows.resize(static_cast<size_t>(p.row_bytes) * p.rows);
				const auto *src = static_cast<const uint8_t *>(mapped) + p.footprint.Offset;
				for (UINT r = 0; r < p.rows; ++r)
					std::memcpy(rows.data() + static_cast<size_t>(r) * static_cast<size_t>(p.row_bytes),
						src + static_cast<size_t>(r) * p.footprint.Footprint.RowPitch, static_cast<size_t>(p.row_bytes));
				const D3D12_RANGE none{ 0, 0 };
				p.readback->Unmap(0, &none);
				if (!write_file(f.dir + L"\\" + widen(files[i]->file), rows.data(), rows.size()))
					error = L"could not write " + f.dir;
			}
			const std::string text = format_captured_frame(f.info);
			if (error.empty() && !write_file(f.dir + L"\\frame.txt", text.data(), text.size()))
				error = L"could not write " + f.dir;
		}
		release(f);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			pending_bytes_ -= bytes;
			if (f.info.index != UINT32_MAX)
				++written_;
			if (!error.empty() && write_error_.empty())
				write_error_ = error;
			writer_busy_ = false;
		}
		wake_.notify_all();
	}
}

std::wstring UpscalerCaptureD3D12::status() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (wanted_ == 0)
		return {};
	if (!write_error_.empty())
		return L"failed: " + write_error_;
	wchar_t buf[256]{};
	if (left_ > 0) {
		_snwprintf_s(buf, _TRUNCATE, L"recording, frame %u of %u", next_index_, wanted_);
		return buf;
	}
	if (written_ < next_index_) {
		_snwprintf_s(buf, _TRUNCATE, L"writing to disk, %u of %u", written_, next_index_);
		return buf;
	}
	if (!status_.empty())
		return status_ + L"; " + std::to_wstring(written_) + L" written to " + root_;
	return L"captured " + std::to_wstring(written_) + L" frames to " + root_;
}

void UpscalerCaptureD3D12::request_scope(const std::wstring &dir, uint32_t frames, uint64_t max_bytes)
{
	if (scope_left_ > 0 || frames == 0 || dir.empty())
		return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (scope_closed_ && !scope_done_)
			return;
		scope_rows_.clear();
		scope_rows_.reserve(frames);
		scope_header_.clear();
		for (CapturedPlane &p : scope_fmt_)
			p = CapturedPlane{};
		scope_members_.assign(frames, UINT32_MAX);
		scope_held_.assign(frames, UINT32_MAX);
		scope_outstanding_ = 0;
		scope_closed_ = false;
		scope_done_ = false;
		scope_frames_written_ = 0;
		scope_note_.clear();
		scope_error_.clear();
	}
	scope_dir_ = dir;
	scope_left_ = frames;
	scope_next_ = 0;
	scope_max_bytes_ = max_bytes;
	scope_sized_ = false;
	scope_frame_bytes_ = 0;
	scope_skipped_ = 0;
	if (!writer_.joinable())
		writer_ = std::thread([this] { writer_main(); });
}

void UpscalerCaptureD3D12::scope_begin(ID3D12Device *device, ID3D12CommandQueue *queue)
{
	collect(device, queue);
}

bool UpscalerCaptureD3D12::scope_size(ID3D12Device *device, const D3D12_RESOURCE_DESC &color)
{
	scope_sized_ = true;
	scope_color_ = color;
	D3D12_RESOURCE_DESC mask = color;
	mask.Format = DXGI_FORMAT_R8_UNORM;
	mask.Flags = D3D12_RESOURCE_FLAG_NONE;
	UINT64 offset = 0;
	CapturedPlane fmt[kScopeStagePlanes];
	for (uint32_t i = 0; i < kScopeStagePlanes; ++i) {
		const D3D12_RESOURCE_DESC &d = static_cast<ScopePlane>(i) == ScopePlane::Mask ? mask : color;
		ScopeSlot &s = scope_slots_[i];
		UINT64 total = 0;
		device->GetCopyableFootprints(&d, 0, 1, 0, &s.footprint, &s.rows, &s.row_bytes, &total);
		if (s.rows == 0 || s.row_bytes == 0 || total == 0)
			return false;
		s.footprint.Offset = offset;
		offset = (offset + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
			~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
		fmt[i].width = static_cast<uint32_t>(d.Width);
		fmt[i].height = d.Height;
		fmt[i].dxgi_format = static_cast<uint32_t>(d.Format);
		fmt[i].row_bytes = static_cast<uint32_t>(s.row_bytes);
	}
	scope_frame_bytes_ = offset;
	const uint64_t fit = scope_max_bytes_ / scope_frame_bytes_;
	uint32_t want = scope_left_;
	if (fit < want)
		want = static_cast<uint32_t>(fit);
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC bd{};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = scope_frame_bytes_;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	scope_pool_.reserve(want);
	while (scope_pool_.size() < want) {
		ID3D12Resource *b = nullptr;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr, IID_PPV_ARGS(&b))))
			break;
		scope_pool_.push_back(b);
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (uint32_t i = 0; i < kScopeStagePlanes; ++i)
			scope_fmt_[i] = fmt[i];
		if (scope_pool_.size() < scope_left_) {
			wchar_t buf[160]{};
			_snwprintf_s(buf, _TRUNCATE, L"shortened to %u frames: %llu MB a frame", scope_next_ +
				static_cast<uint32_t>(scope_pool_.size()), static_cast<unsigned long long>(scope_frame_bytes_ >> 20));
			scope_note_ = buf;
		}
	}
	if (scope_pool_.size() < scope_left_)
		scope_left_ = static_cast<uint32_t>(scope_pool_.size());
	return !scope_pool_.empty();
}

void UpscalerCaptureD3D12::scope_copy(ScopePlane plane, ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *res, D3D12_RESOURCE_STATES state)
{
	const uint32_t i = static_cast<uint32_t>(plane);
	if (scope_left_ == 0 || i >= kScopeStagePlanes || device == nullptr || cmd == nullptr || res == nullptr ||
		(scope_cur_.scope_planes & (1u << i)) != 0)
		return;
	const D3D12_RESOURCE_DESC desc = res->GetDesc();
	if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1)
		return;
	if (!scope_sized_) {
		if (plane == ScopePlane::Mask)
			return;
		if (!scope_size(device, desc)) {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				scope_error_ = L"the readback memory could not be made";
			}
			scope_close_all();
			return;
		}
	}
	const bool fits = plane == ScopePlane::Mask
		? desc.Width == scope_color_.Width && desc.Height == scope_color_.Height &&
			desc.Format == DXGI_FORMAT_R8_UNORM
		: desc.Width == scope_color_.Width && desc.Height == scope_color_.Height &&
			desc.Format == scope_color_.Format;
	if (!fits) {
		++scope_skipped_;
		return;
	}
	if (scope_cur_.scope_buffer == nullptr) {
		if (scope_pool_.empty()) {
			++scope_skipped_;
			return;
		}
		scope_cur_.scope_buffer = scope_pool_.back();
		scope_pool_.pop_back();
		scope_cur_.scope = true;
		for (uint32_t k = 0; k < kScopeStagePlanes; ++k)
			scope_cur_.slots[k] = scope_slots_[k];
	}
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = scope_cur_.scope_buffer;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = scope_slots_[i].footprint;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = res;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	const bool transition = state != D3D12_RESOURCE_STATE_COMMON;
	if (transition)
		barrier12(cmd, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	if (transition)
		barrier12(cmd, res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
	scope_cur_.scope_planes |= 1u << i;
}

void UpscalerCaptureD3D12::scope_end(ScopeTraceRow row, const std::string &header)
{
	if (scope_left_ == 0)
		return;
	const uint32_t n = scope_next_++;
	row.set(ScopeColumn::N, static_cast<double>(n));
	if (scope_cur_.scope_buffer != nullptr) {
		scope_cur_.n = n;
		scope_cur_.dir = scope_dir_;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			++scope_outstanding_;
		}
		in_flight_.push_back(std::move(scope_cur_));
		scope_cur_ = Frame{};
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		scope_rows_.push_back(std::move(row));
		scope_header_ = header;
	}
	if (--scope_left_ == 0)
		scope_close_all();
}

void UpscalerCaptureD3D12::scope_close_all()
{
	if (scope_cur_.scope_buffer != nullptr) {
		scope_cur_.n = UINT32_MAX;
		in_flight_.push_back(std::move(scope_cur_));
		scope_cur_ = Frame{};
	}
	for (ID3D12Resource *b : scope_pool_)
		b->Release();
	scope_pool_.clear();
	scope_left_ = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!scope_done_)
			scope_closed_ = true;
		if (scope_skipped_ != 0) {
			const std::wstring skipped = std::to_wstring(scope_skipped_) + L" planes skipped (size or memory)";
			scope_note_ = scope_note_.empty() ? skipped : scope_note_ + L", " + skipped;
		}
	}
	wake_.notify_all();
}

bool UpscalerCaptureD3D12::scope_finalize_due() const
{
	return scope_closed_ && !scope_done_ && scope_outstanding_ == 0;
}

void UpscalerCaptureD3D12::scope_finalize()
{
	std::vector<ScopeTraceRow> rows;
	std::string header;
	CapturedPlane fmt[kScopeStagePlanes];
	std::vector<uint32_t> members, held;
	std::wstring dir;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		rows = scope_rows_;
		header = scope_header_;
		for (uint32_t i = 0; i < kScopeStagePlanes; ++i)
			fmt[i] = scope_fmt_[i];
		members = scope_members_;
		held = scope_held_;
		dir = scope_dir_;
	}
	std::wstring error;
	make_directories(dir);
	for (ScopeTraceRow &r : rows) {
		const double n = r.get(ScopeColumn::N);
		const size_t k = n >= 0.0 ? static_cast<size_t>(n) : SIZE_MAX;
		if (k < members.size() && members[k] != UINT32_MAX) {
			r.set(ScopeColumn::RestoreMembers, members[k]);
			r.set(ScopeColumn::RestoreHeld, held[k]);
		}
	}
	for (uint32_t i = 0; i < kScopeStagePlanes; ++i) {
		if (fmt[i].file.empty())
			continue;
		const std::string line = format_scope_fmt(fmt[i].width, fmt[i].height, fmt[i].dxgi_format, fmt[i].row_bytes);
		if (!write_file(dir + L"\\" + widen(fmt[i].file) + L".fmt", line.data(), line.size()))
			error = L"could not write " + dir;
	}
	std::string trace = format_scope_trace_header();
	for (const ScopeTraceRow &r : rows)
		trace += format_scope_trace_row(r);
	if (!write_file(dir + L"\\trace.csv", trace.data(), trace.size()))
		error = L"could not write " + dir;
	const std::string scope = format_scope_txt(header, static_cast<uint32_t>(rows.size()));
	if (!write_file(dir + L"\\scope.txt", scope.data(), scope.size()))
		error = L"could not write " + dir;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!error.empty() && scope_error_.empty())
		scope_error_ = error;
	scope_done_ = true;
}

std::wstring UpscalerCaptureD3D12::scope_status() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (scope_dir_.empty())
		return {};
	if (!scope_error_.empty())
		return L"failed: " + scope_error_;
	wchar_t buf[256]{};
	if (!scope_closed_) {
		_snwprintf_s(buf, _TRUNCATE, L"recording, frame %u of %u", scope_next_, scope_next_ + scope_left_);
		return buf;
	}
	if (!scope_done_) {
		_snwprintf_s(buf, _TRUNCATE, L"writing to disk, %u frames with planes left", scope_outstanding_);
		return buf;
	}
	const std::wstring done = L"captured " + std::to_wstring(scope_rows_.size()) + L" frames to " + scope_dir_;
	return scope_note_.empty() ? done : done + L" (" + scope_note_ + L")";
}

}

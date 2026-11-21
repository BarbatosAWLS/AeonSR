#pragma once

#include "aeon_sr/core/runtime_load_scope.hpp"
#include "aeon_sr/ngx/ngx_dlssnr.hpp"
#include "aeon_sr/ngx/neural_hardware.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aeon_sr {

class NeuralLoader {
public:
	enum class Step {
		Run,
		Wait,
		Failed,
	};
	static constexpr uint64_t kSettleMs = 1000, kRetryMs = 2000;

	NeuralLoader() = default;
	NeuralLoader(const NeuralLoader &) = delete;
	NeuralLoader &operator=(const NeuralLoader &) = delete;
	~NeuralLoader() { join(); }

	bool loading() const noexcept { return state_.load(std::memory_order_acquire) == kRunning; }

	void join(bool *init_failed = nullptr)
	{
		if (thread_.joinable())
			thread_.join();
		if (state_.load(std::memory_order_acquire) == kFinished && !ok_ && !feature_ && init_failed != nullptr)
			*init_failed = true;
		state_.store(kIdle, std::memory_order_release);
		feature_ = false;
		landed_ms_ = 0;
		failed_ms_ = 0;
	}

	Step step(NeuralRenderD3D12 &nr, ID3D12Device *device, ID3D12CommandQueue *queue, uint32_t init_w,
		uint32_t init_h, ID3D12Resource *color, const NeuralRenderParams &np, bool *init_failed)
	{
		const int state = state_.load(std::memory_order_acquire);
		if (state == kRunning)
			return Step::Wait;
		const uint64_t now = GetTickCount64();
		if (state == kFinished) {
			const bool was_feature = feature_;
			if (thread_.joinable())
				thread_.join();
			state_.store(kIdle, std::memory_order_release);
			feature_ = false;
			if (!ok_) {
				if (was_feature)
					failed_ms_ = now;
				else
					*init_failed = true;
				return Step::Failed;
			}
			if (was_feature) {
				landed_ms_ = now;
				failed_ms_ = 0;
			}
		}
		if (*init_failed || device == nullptr)
			return Step::Failed;
		if (nr.initialized && nr.device != device)
			nr.shutdown_device();
		if (!nr.initialized) {
			if (!nr.ensure_dll_present()) {
				*init_failed = true;
				return Step::Failed;
			}
			nr.set_status(UpscalerStatus::Loading,
				L"DLSS neural rendering is starting on a thread of its own; the frame goes on without it");
			NeuralRenderD3D12 *const pass = &nr;
			start([pass, device, init_w, init_h] { return pass->init_device(device, init_w, init_h); }, false);
			return Step::Wait;
		}
		nr.set_command_queue(queue);
		if (color == nullptr || queue == nullptr || nr.crashed || nr.feature_unsupported)
			return Step::Run;
		const D3D12_RESOURCE_DESC desc = color->GetDesc();
		const uint32_t w = static_cast<uint32_t>(desc.Width), h = desc.Height;
		if (w == 0 || h == 0 || nr.feature_ready(w, h, desc.Format, np))
			return Step::Run;
		if (failed_ms_ != 0 && now - failed_ms_ < kRetryMs)
			return Step::Failed;
		if (landed_ms_ != 0 && now - landed_ms_ < kSettleMs)
			return Step::Run;
		nr.set_status(UpscalerStatus::Loading,
			L"DLSS neural rendering is building its passes on a thread of its own; the frame goes on without it");
		NeuralRenderD3D12 *const pass = &nr;
		const DXGI_FORMAT src = desc.Format;
		const NeuralRenderParams p = np;
		start([pass, w, h, src, p] { return pass->build_feature(w, h, src, p); }, true);
		return Step::Wait;
	}

private:
	static constexpr int kIdle = 0, kRunning = 1, kFinished = 2;

	template <class Work>
	void start(Work work, bool feature)
	{
		feature_ = feature;
		ok_ = false;
		g_neural_building.fetch_add(1, std::memory_order_acq_rel);
		state_.store(kRunning, std::memory_order_release);
		try {
			thread_ = std::thread([this, work, feature] {
				if (feature) {
					ok_ = work();
				} else {
					const RuntimeLoadScope loading;
					ok_ = work();
				}
				g_neural_building.fetch_sub(1, std::memory_order_acq_rel);
				state_.store(kFinished, std::memory_order_release);
			});
		} catch (...) {
			g_neural_building.fetch_sub(1, std::memory_order_acq_rel);
			state_.store(kFinished, std::memory_order_release);
		}
	}

	std::thread thread_;
	std::atomic<int> state_{ kIdle };
	bool ok_ = false;
	bool feature_ = false;
	uint64_t landed_ms_ = 0, failed_ms_ = 0;
};

class NeuralModelPeek {
public:
	NeuralModelPeek() = default;
	NeuralModelPeek(const NeuralModelPeek &) = delete;
	NeuralModelPeek &operator=(const NeuralModelPeek &) = delete;
	~NeuralModelPeek() { join(); }

	void start(const std::wstring &path)
	{
		if constexpr (sizeof(void *) == 4)
			return;
		if (path.empty() || running_.load(std::memory_order_acquire))
			return;
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			if (path == path_)
				return;
			path_ = path;
		}
		if (thread_.joinable())
			thread_.join();
		running_.store(true, std::memory_order_release);
		try {
			thread_ = std::thread([this, path] {
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
				NeuralRuntimeKernels kernels;
				NeuralRuntimeIdentity identity;
				std::string sha;
				std::vector<uint32_t> ids;
				if (neural_read_runtime_file(path, &kernels, &identity, &sha))
					ids = neural_model_ids(identity.models);
				std::wstring letters;
				for (const uint32_t id : ids) {
					letters += letters.empty() ? L" " : L", ";
					letters += static_cast<wchar_t>(neural_model_letter(id));
				}
				diag_logf(DiagLevel::Info, "neural", L"%s carries %zu model(s)%s, read for the panel before the pass "
					L"started", path.c_str(), ids.size(), letters.c_str());
				{
					const std::lock_guard<std::mutex> lock(mutex_);
					ids_ = std::move(ids);
				}
				running_.store(false, std::memory_order_release);
			});
		} catch (...) {
			running_.store(false, std::memory_order_release);
		}
	}

	std::vector<uint32_t> models() const
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		return ids_;
	}

	void join()
	{
		if (thread_.joinable())
			thread_.join();
	}

	void abandon()
	{
		if (thread_.joinable())
			thread_.detach();
	}

private:
	std::thread thread_;
	std::atomic<bool> running_{ false };
	mutable std::mutex mutex_;
	std::wstring path_;
	std::vector<uint32_t> ids_;
};

}

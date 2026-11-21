#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace aeon_sr {

struct LoadProcess {
	double cpu_ms = -1.0;
	double private_mb = -1.0;
	int64_t handles = -1;
	double gpu_usage_mb = -1.0;
	double gpu_budget_mb = -1.0;
	double engine_wait_ms = -1.0;
	uint64_t engine_made = 0;
	bool holds_on = false;
	bool framegen_on = false;
};

struct LoadKept {
	static constexpr uint32_t kMax = 12;
	const char *name[kMax]{};
	uint64_t count[kMax]{};
	uint32_t n = 0;
	void add(const char *what, uint64_t c) noexcept
	{
		if (n < kMax) {
			name[n] = what;
			count[n] = c;
			++n;
		}
	}
};

class LoadWindow {
public:
	static constexpr uint64_t kPeriodMs = 60000;
	static constexpr double kSlowMs = 50.0;

	enum class Step {
		Baseline,
		Counted,
		Due,
	};

	void add_work(double ms) noexcept { work_ += ms; }
	void add_held(double ms) noexcept { held_ += ms; }
	void add_framegen(double ms) noexcept { framegen_ += ms; }

	Step present(uint64_t now_ms, double frame_ms) noexcept
	{
		if (!have_before_) {
			clear_pending();
			return Step::Baseline;
		}
		if (!started_) {
			start(now_ms);
			clear_pending();
			return Step::Counted;
		}
		++frames_;
		frame_sum_ += frame_ms;
		frame_max_ = frame_ms > frame_max_ ? frame_ms : frame_max_;
		slow_ += frame_ms > kSlowMs ? 1u : 0u;
		work_sum_ += work_;
		work_max_ = work_ > work_max_ ? work_ : work_max_;
		held_sum_ += held_;
		held_max_ = held_ > held_max_ ? held_ : held_max_;
		framegen_sum_ += framegen_;
		framegen_max_ = framegen_ > framegen_max_ ? framegen_ : framegen_max_;
		clear_pending();
		return now_ms - since_ >= kPeriodMs ? Step::Due : Step::Counted;
	}

	void baseline(const LoadProcess &process) noexcept
	{
		before_ = process;
		have_before_ = true;
		started_ = false;
	}

	uint32_t frames() const noexcept { return frames_; }

	std::wstring take(uint64_t now_ms, const LoadProcess &process, const LoadKept &kept)
	{
		const double n = frames_ != 0 ? static_cast<double>(frames_) : 1.0;
		const double seconds = static_cast<double>(now_ms - since_) / 1000.0;
		std::wstring line;
		wchar_t buf[512]{};
		_snwprintf_s(buf, _TRUNCATE, L"load: last %.0f s %u frames, %.2f ms a frame (the longest %.1f ms, %u over %.0f ms); "
			L"the add-on's work %.2f ms a frame on the present thread (the longest %.2f ms)", seconds, frames_,
			frame_sum_ / n, frame_max_, slow_, kSlowMs, work_sum_ / n, work_max_);
		line += buf;
		if (process.holds_on || held_sum_ > 0.0) {
			_snwprintf_s(buf, _TRUNCATE, L", its holds %.2f ms a frame (the longest %.2f ms)", held_sum_ / n, held_max_);
			line += buf;
		}
		if (process.framegen_on || framegen_sum_ > 0.0) {
			_snwprintf_s(buf, _TRUNCATE, L", frame generation %.2f ms a frame (the longest %.2f ms)", framegen_sum_ / n,
				framegen_max_);
			line += buf;
		}
		if (process.engine_wait_ms >= 0.0) {
			if (before_.engine_wait_ms >= 0.0 && before_.engine_made == process.engine_made &&
				process.engine_wait_ms >= before_.engine_wait_ms) {
				_snwprintf_s(buf, _TRUNCATE, L", %.2f ms waiting for the engine's GPU",
					(process.engine_wait_ms - before_.engine_wait_ms) / n);
				line += buf;
			} else {
				line += before_.engine_wait_ms < 0.0 ? L", the engine made in this minute (its wait not counted)"
					: L", the engine made again in this minute (its wait not counted)";
			}
		}
		line += L"; the process";
		bool first = true;
		const auto part = [&](const wchar_t *text) {
			line += first ? L" " : L", ";
			line += text;
			first = false;
		};
		if (process.cpu_ms >= 0.0 && before_.cpu_ms >= 0.0 && process.cpu_ms >= before_.cpu_ms) {
			_snwprintf_s(buf, _TRUNCATE, L"%.2f ms of CPU a frame", (process.cpu_ms - before_.cpu_ms) / n);
			part(buf);
		}
		if (process.private_mb >= 0.0) {
			_snwprintf_s(buf, _TRUNCATE, L"%.0f MB private", process.private_mb);
			part(buf);
		}
		if (process.handles >= 0) {
			_snwprintf_s(buf, _TRUNCATE, L"%lld handles", static_cast<long long>(process.handles));
			part(buf);
		}
		if (process.gpu_usage_mb >= 0.0) {
			if (process.gpu_budget_mb > 0.0)
				_snwprintf_s(buf, _TRUNCATE, L"GPU memory %.0f MB of a budget of %.0f MB", process.gpu_usage_mb,
					process.gpu_budget_mb);
			else
				_snwprintf_s(buf, _TRUNCATE, L"GPU memory %.0f MB", process.gpu_usage_mb);
			part(buf);
		}
		if (first)
			line += L" not read";
		if (kept.n != 0) {
			line += L"; kept:";
			for (uint32_t i = 0; i < kept.n; ++i) {
				_snwprintf_s(buf, _TRUNCATE, L"%ls %llu %hs", i == 0 ? L"" : L",",
					static_cast<unsigned long long>(kept.count[i]), kept.name[i] != nullptr ? kept.name[i] : "?");
				line += buf;
			}
		}
		before_ = process;
		start(now_ms);
		return line;
	}

private:
	void clear_pending() noexcept { work_ = held_ = framegen_ = 0.0; }
	void start(uint64_t now_ms) noexcept
	{
		started_ = true;
		since_ = now_ms;
		frames_ = 0;
		slow_ = 0;
		frame_sum_ = frame_max_ = 0.0;
		work_sum_ = work_max_ = held_sum_ = held_max_ = framegen_sum_ = framegen_max_ = 0.0;
	}

	uint64_t since_ = 0;
	bool started_ = false;
	uint32_t frames_ = 0, slow_ = 0;
	double frame_sum_ = 0.0, frame_max_ = 0.0;
	double work_ = 0.0, held_ = 0.0, framegen_ = 0.0;
	double work_sum_ = 0.0, work_max_ = 0.0;
	double held_sum_ = 0.0, held_max_ = 0.0;
	double framegen_sum_ = 0.0, framegen_max_ = 0.0;
	LoadProcess before_{};
	bool have_before_ = false;
};

}

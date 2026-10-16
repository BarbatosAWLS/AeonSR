#include "aeon_sr/core/flight_recorder.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aeon_sr {
namespace {

template <typename E, size_t N>
const char *name_in(const char *const (&names)[N], E value) noexcept
{
	const size_t i = static_cast<size_t>(value);
	return i < N ? names[i] : "";
}

void put_float(std::string &out, float v)
{
	if (std::isfinite(v)) {
		char b[32];
		const int n = std::snprintf(b, sizeof(b), "%.4f", static_cast<double>(v));
		if (n > 0)
			out.append(b, static_cast<size_t>(n));
	}
	out += ',';
}

void put_uint(std::string &out, uint64_t v)
{
	char b[24];
	const int n = std::snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(v));
	if (n > 0)
		out.append(b, static_cast<size_t>(n));
	out += ',';
}

void put_size(std::string &out, uint16_t v)
{
	if (v != 0)
		put_uint(out, v);
	else
		out += ',';
}

void put_text(std::string &out, const char *s)
{
	if (s == nullptr)
		s = "";
	if (std::strpbrk(s, ",\"\r\n") == nullptr) {
		out += s;
	} else {
		out += '"';
		for (const char *p = s; *p != 0; ++p) {
			if (*p == '"')
				out += '"';
			out += (*p == '\r' || *p == '\n') ? ' ' : *p;
		}
		out += '"';
	}
	out += ',';
}

bool is_failure(uint8_t status) noexcept
{
	return status == 4u || status == 7u || status == 8u;
}

}

const char *flight_api_name(FlightApi a) noexcept
{
	static const char *const n[] = { "", "d3d9", "d3d10", "d3d11", "d3d12", "opengl", "vulkan" };
	static_assert(std::size(n) == static_cast<size_t>(FlightApi::Count));
	return name_in(n, a);
}

const char *flight_upscaler_name(FlightUpscaler u) noexcept
{
	static const char *const n[] = { "none", "dlss", "dlss_d3d11", "fsr", "fsr_d3d11", "xess", "vsr" };
	static_assert(std::size(n) == static_cast<size_t>(FlightUpscaler::Count));
	return name_in(n, u);
}

const char *flight_jitter_name(FlightJitter j) noexcept
{
	static const char *const n[] = { "idle", "off", "drawn", "split", "ambiguous", "empty", "resampled", "paused",
		"withheld", "not_drawn" };
	static_assert(std::size(n) == static_cast<size_t>(FlightJitter::Count));
	return name_in(n, j);
}

const char *flight_scene_name(FlightScene s) noexcept
{
	static const char *const n[] = { "", "depth", "size", "everything" };
	static_assert(std::size(n) == static_cast<size_t>(FlightScene::Count));
	return name_in(n, s);
}

const char *flight_host_name(FlightHost h) noexcept
{
	static const char *const n[] = { "", "answered", "timeout", "busy", "lost", "failed" };
	static_assert(std::size(n) == static_cast<size_t>(FlightHost::Count));
	return name_in(n, h);
}

const char *flight_blocked_name(FlightBlocked b) noexcept
{
	static const char *const n[] = { "", "device_lost", "no_bridge", "no_back_buffer", "bridge_failed" };
	static_assert(std::size(n) == static_cast<size_t>(FlightBlocked::Count));
	return name_in(n, b);
}

const char *flight_game_jitter_name(FlightGameJitter g) noexcept
{
	static const char *const n[] = { "", "no", "yes" };
	static_assert(std::size(n) == static_cast<size_t>(FlightGameJitter::Count));
	return name_in(n, g);
}

const char *flight_trigger_name(FlightTrigger t) noexcept
{
	static const char *const n[] = { "jitter_flap", "jitter_pause", "landing_low", "game_jitter", "host_slow",
		"failure", "frame_spike", "shutdown", "report" };
	static_assert(std::size(n) == static_cast<size_t>(FlightTrigger::Count));
	return name_in(n, t);
}

const char *flight_status_name(uint8_t status) noexcept
{
	static const char *const n[] = { "idle", "missing_runtime", "unsupported_api", "unsupported_gpu", "init_failed",
		"need_motion_vectors", "ready", "evaluate_failed", "crashed", "need_depth", "loading" };
	return status < std::size(n) ? n[status] : "";
}

const char *flight_plan_name(uint8_t plan) noexcept
{
	static const char *const n[] = { "off", "drawn", "split", "ambiguous", "empty", "resampled" };
	return plan < std::size(n) ? n[plan] : "";
}

uint8_t FlightNotes::id(const char *text) noexcept
{
	if (text == nullptr || text[0] == 0)
		return 0;
	for (uint32_t i = 1; i < count_; ++i) {
		if (ptr_[i] == text && std::strncmp(text_[i], text, kChars - 1u) == 0)
			return static_cast<uint8_t>(i);
	}
	for (uint32_t i = 1; i < count_; ++i) {
		if (std::strncmp(text_[i], text, kChars - 1u) == 0) {
			ptr_[i] = text;
			return static_cast<uint8_t>(i);
		}
	}
	if (count_ >= kMax)
		return kFull;
	ptr_[count_] = text;
	strncpy_s(text_[count_], kChars, text, _TRUNCATE);
	return static_cast<uint8_t>(count_++);
}

const char *FlightNotes::text(uint8_t id) const noexcept
{
	if (id == kFull)
		return "(more reasons than the table holds)";
	return id < count_ ? text_[id] : "";
}

uint16_t FlightDetector::detect(const FlightRecord &r) noexcept
{
	uint16_t fired = 0;
	const auto fire = [&fired](FlightTrigger t) { fired = static_cast<uint16_t>(fired | flight_bit(t)); };

	const int cls = r.jitter == FlightJitter::Split ? static_cast<int>(FlightJitter::Drawn) : static_cast<int>(r.jitter);
	if (flap_class_ >= 0 && cls != flap_class_) {
		changes_[change_next_++ % kChangeSlots] = r.time_ms;
		uint32_t recent = 0;
		for (double t : changes_)
			recent += t != 0.0 && r.time_ms - t <= kFlapWindowMs ? 1u : 0u;
		if (recent >= kFlapChanges) {
			fire(FlightTrigger::JitterFlap);
			std::fill(std::begin(changes_), std::end(changes_), 0.0);
		}
	}
	flap_class_ = cls;

	const bool paused = r.jitter == FlightJitter::Paused;
	if (paused && !paused_)
		fire(FlightTrigger::JitterPause);
	paused_ = paused;

	if ((r.jitter == FlightJitter::Drawn || r.jitter == FlightJitter::Split) &&
		(r.landing_x < kLandingLow || r.landing_y < kLandingLow))
		fire(FlightTrigger::LandingLow);

	if (r.game_jitter != game_ && r.game_jitter != FlightGameJitter::Unmeasured &&
		!(game_ == FlightGameJitter::Unmeasured && r.game_jitter == FlightGameJitter::No))
		fire(FlightTrigger::GameJitter);
	if (r.game_jitter != FlightGameJitter::Unmeasured)
		game_ = r.game_jitter;

	if (r.host == FlightHost::Timeout || r.host == FlightHost::Busy || r.host == FlightHost::Lost ||
		r.host == FlightHost::Failed || r.host_ms > kHostSlowMs)
		fire(FlightTrigger::HostSlow);

	const bool failed = is_failure(r.status) && r.upscaler != FlightUpscaler::None;
	const bool neural_failed = r.neural < 0;
	if ((failed && !failed_) || (neural_failed && !neural_failed_) ||
		(r.blocked != FlightBlocked::None && r.blocked != blocked_))
		fire(FlightTrigger::Failure);
	failed_ = failed;
	neural_failed_ = neural_failed;
	blocked_ = r.blocked;

	if (std::isfinite(r.frame_ms) && r.frame_ms > 0.0f) {
		if (timed_ >= kSpikeWarmup && r.frame_ms > kSpikeRatio * mean_ms_ && r.frame_ms > mean_ms_ + kSpikeAboveMs)
			fire(FlightTrigger::FrameSpike);
		const float fed = timed_ == 0 ? r.frame_ms : std::min(r.frame_ms, 2.0f * mean_ms_);
		mean_ms_ = timed_ == 0 ? fed : mean_ms_ + (fed - mean_ms_) / 32.0f;
		++timed_;
	}
	return fired;
}

bool FlightPolicy::offer(uint16_t triggers, uint64_t frame, double t_ms) noexcept
{
	bool taken = false;
	for (uint32_t k = 0; k < static_cast<uint32_t>(FlightTrigger::Count); ++k) {
		if ((triggers & (1u << k)) == 0)
			continue;
		const FlightTrigger kind = static_cast<FlightTrigger>(k);
		const bool limited = kind != FlightTrigger::Report && kind != FlightTrigger::Shutdown;
		if (limited && seen_[k] && t_ms - last_ms_[k] < kFlightEveryMs)
			continue;
		seen_[k] = true;
		last_ms_[k] = t_ms;
		taken = true;
		if (!pending_.active) {
			pending_.active = true;
			pending_.first = kind;
			pending_.kinds = 0;
			pending_.frame = frame;
			pending_.trigger_ms = t_ms;
			pending_.due_ms = t_ms + kFlightAfterMs;
		}
		pending_.kinds = static_cast<uint16_t>(pending_.kinds | (1u << k));
	}
	return taken;
}

FlightPolicy::Pending FlightPolicy::take() noexcept
{
	const Pending p = pending_;
	pending_ = Pending{};
	return p;
}

std::wstring flight_file_name(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute,
	uint32_t second, uint32_t ms, FlightTrigger kind)
{
	wchar_t b[96]{};
	_snwprintf_s(b, _TRUNCATE, L"flight-%04u%02u%02u-%02u%02u%02u-%03u-%hs.csv", year, month, day, hour, minute,
		second, ms, flight_trigger_name(kind));
	return b;
}

std::wstring flight_file_kind(const std::wstring &name)
{
	constexpr size_t kPrefix = 27;
	if (name.size() <= kPrefix + 4 || name.compare(0, 7, L"flight-") != 0 ||
		name.compare(name.size() - 4, 4, L".csv") != 0 || name[15] != L'-' || name[22] != L'-' || name[26] != L'-')
		return {};
	return name.substr(kPrefix, name.size() - kPrefix - 4);
}

std::vector<std::wstring> flight_prune(std::vector<FlightFile> files, FlightTrigger kind, uint64_t incoming,
	uint32_t max_files, uint32_t max_per_kind, uint64_t max_bytes)
{
	files.erase(std::remove_if(files.begin(), files.end(),
		[](const FlightFile &f) { return flight_file_kind(f.name).empty(); }), files.end());
	std::sort(files.begin(), files.end(), [](const FlightFile &a, const FlightFile &b) { return a.name < b.name; });
	const std::string k8 = flight_trigger_name(kind);
	const std::wstring wkind(k8.begin(), k8.end());
	std::vector<std::wstring> gone;
	size_t of_kind = 0;
	for (const FlightFile &f : files)
		of_kind += flight_file_kind(f.name) == wkind ? 1u : 0u;
	for (auto it = files.begin(); it != files.end() && of_kind + 1u > max_per_kind;) {
		if (flight_file_kind(it->name) == wkind) {
			gone.push_back(it->name);
			it = files.erase(it);
			--of_kind;
		} else {
			++it;
		}
	}
	uint64_t bytes = incoming;
	for (const FlightFile &f : files)
		bytes += f.bytes;
	while (!files.empty() && (files.size() + 1u > max_files || bytes > max_bytes)) {
		bytes -= files.front().bytes;
		gone.push_back(files.front().name);
		files.erase(files.begin());
	}
	return gone;
}

std::string flight_csv_header()
{
	return "frame,time_ms,frame_ms,api,width,height,upscaler,status,ran,render_w,render_h,neural,neural_ms,"
		"jitter,plan,note,pause_reason,drawn_index,drawn_x,drawn_y,told_x,told_y,moved,plain,landing_x,landing_y,"
		"game_jitter,game_depth_x,game_depth_y,game_color_x,game_color_y,game_rms,scene,depth_id,depth_w,depth_h,"
		"interface,host,host_ms,blocked,triggers\n";
}

void flight_csv_row(std::string &out, const FlightRecord &r, const FlightNotes &notes)
{
	put_uint(out, r.frame);
	{
		char b[32];
		const int n = std::snprintf(b, sizeof(b), "%.3f,", r.time_ms);
		if (n > 0)
			out.append(b, static_cast<size_t>(n));
	}
	put_float(out, r.frame_ms);
	put_text(out, flight_api_name(r.api));
	put_size(out, r.width);
	put_size(out, r.height);
	put_text(out, flight_upscaler_name(r.upscaler));
	put_text(out, r.upscaler != FlightUpscaler::None ? flight_status_name(r.status) : "");
	if (r.ran >= 0)
		put_uint(out, static_cast<uint64_t>(r.ran));
	else
		out += ',';
	put_size(out, r.render_w);
	put_size(out, r.render_h);
	out += r.neural > 0 ? "ran," : r.neural < 0 ? "failed," : ",";
	put_float(out, r.neural_ms);
	put_text(out, flight_jitter_name(r.jitter));
	put_text(out, flight_plan_name(r.plan));
	put_text(out, notes.text(r.note));
	put_text(out, notes.text(r.pause));
	if (std::isfinite(r.drawn_x))
		put_uint(out, r.drawn_index);
	else
		out += ',';
	put_float(out, r.drawn_x);
	put_float(out, r.drawn_y);
	put_float(out, r.told_x);
	put_float(out, r.told_y);
	put_uint(out, r.moved);
	put_uint(out, r.plain);
	put_float(out, r.landing_x);
	put_float(out, r.landing_y);
	put_text(out, flight_game_jitter_name(r.game_jitter));
	put_float(out, r.game_depth_x);
	put_float(out, r.game_depth_y);
	put_float(out, r.game_color_x);
	put_float(out, r.game_color_y);
	put_float(out, r.game_rms);
	put_text(out, flight_scene_name(r.scene));
	if (r.depth_id != 0) {
		char b[24];
		const int n = std::snprintf(b, sizeof(b), "%llx,", static_cast<unsigned long long>(r.depth_id));
		if (n > 0)
			out.append(b, static_cast<size_t>(n));
	} else {
		out += ',';
	}
	put_size(out, r.depth_w);
	put_size(out, r.depth_h);
	{
		std::string hud;
		if (r.hud & kFlightRestore)
			hud += "restore";
		if (r.hud & kFlightSplit)
			hud += hud.empty() ? "split" : "+split";
		if (r.hud & kFlightRegistered)
			hud += hud.empty() ? "registered" : "+registered";
		if (r.hud & kFlightAlone)
			hud += hud.empty() ? "alone" : "+alone";
		if (r.hud & kFlightHeld)
			hud += hud.empty() ? "held" : "+held";
		put_text(out, hud.c_str());
	}
	put_text(out, flight_host_name(r.host));
	put_float(out, r.host_ms);
	put_text(out, flight_blocked_name(r.blocked));
	std::string fired;
	for (uint32_t k = 0; k < static_cast<uint32_t>(FlightTrigger::Count); ++k) {
		if ((r.triggers & (1u << k)) != 0)
			fired += std::string(fired.empty() ? "" : "+") + flight_trigger_name(static_cast<FlightTrigger>(k));
	}
	out += fired;
	out += '\n';
}

uint16_t FlightFault::at(uint64_t f) const noexcept
{
	uint16_t bits = 0;
	for (uint32_t i = 0; i < count && i < kMax; ++i) {
		if (frame[i] == f)
			bits = static_cast<uint16_t>(bits | flight_bit(kind[i]));
	}
	return bits;
}

bool flight_trigger_of(const std::string &name, FlightTrigger *out) noexcept
{
	for (uint32_t k = 0; k < static_cast<uint32_t>(FlightTrigger::Count); ++k) {
		if (name == flight_trigger_name(static_cast<FlightTrigger>(k))) {
			*out = static_cast<FlightTrigger>(k);
			return true;
		}
	}
	return false;
}

bool parse_flight_fault(const std::string &text, FlightFault *out)
{
	FlightFault f;
	if (text.empty() || text == "none") {
		*out = f;
		return true;
	}
	for (size_t from = 0; from <= text.size();) {
		const size_t plus = text.find('+', from);
		const std::string one = text.substr(from, plus == std::string::npos ? std::string::npos : plus - from);
		const size_t colon = one.find(':');
		if (colon == std::string::npos || colon + 1 >= one.size() || f.count >= FlightFault::kMax)
			return false;
		FlightTrigger kind{};
		if (!flight_trigger_of(one.substr(0, colon), &kind))
			return false;
		const std::string digits = one.substr(colon + 1);
		if (digits.find_first_not_of("0123456789") != std::string::npos)
			return false;
		f.kind[f.count] = kind;
		f.frame[f.count] = std::strtoull(digits.c_str(), nullptr, 10);
		++f.count;
		if (plus == std::string::npos)
			break;
		from = plus + 1;
	}
	*out = f;
	return true;
}

FlightRecorder::FlightRecorder()
	: ring_(kFlightRing), dump_(kFlightRing)
{
	jobs_.reserve(8);
}

FlightRecorder::~FlightRecorder()
{
	{
		std::lock_guard<std::mutex> lock(jobs_lock_);
		quit_ = true;
	}
	wake_.notify_all();
	if (writer_.joinable())
		writer_.join();
}

void FlightRecorder::set_dir(const std::wstring &dir)
{
	{
		std::lock_guard<std::mutex> lock(lock_);
		dir_ = dir;
	}
	start_writer();
}

void FlightRecorder::start_writer()
{
	std::lock_guard<std::mutex> lock(jobs_lock_);
	if (!writer_.joinable() && !quit_)
		writer_ = std::thread([this] { writer_main(); });
}

uint32_t FlightRecorder::snapshot(double since_ms, FlightRecord *out, uint32_t room) const noexcept
{
	const uint64_t held = head_ < kFlightRing ? head_ : kFlightRing;
	uint64_t first = head_ - held;
	while (first < head_ && ring_[first % kFlightRing].time_ms < since_ms)
		++first;
	if (head_ - first > room)
		first = head_ - room;
	uint32_t n = 0;
	for (uint64_t i = first; i < head_; ++i)
		out[n++] = ring_[i % kFlightRing];
	return n;
}

void FlightRecorder::commit(int64_t qpc, int64_t qpf, uint16_t forced, const char *note, const char *pause) noexcept
{
	static const int64_t freq = [] {
		LARGE_INTEGER f{};
		QueryPerformanceFrequency(&f);
		return static_cast<int64_t>(f.QuadPart);
	}();
	LARGE_INTEGER start{};
	QueryPerformanceCounter(&start);
	std::lock_guard<std::mutex> lock(lock_);
	if (stopped_ || qpf <= 0) {
		cur_ = FlightRecord{};
		return;
	}
	FlightRecord &r = cur_;
	if (head_ == 0)
		first_qpc_ = qpc;
	r.time_ms = static_cast<double>(qpc - first_qpc_) * 1000.0 / static_cast<double>(qpf);
	if (head_ != 0)
		r.frame_ms = static_cast<float>(static_cast<double>(qpc - last_qpc_) * 1000.0 / static_cast<double>(qpf));
	last_qpc_ = qpc;
	r.frame = head_;
	r.note = notes_.id(note);
	r.pause = notes_.id(pause);
	r.triggers = static_cast<uint16_t>(detector_.detect(r) | forced);
	ring_[head_ % kFlightRing] = r;
	++head_;
	if (r.triggers != 0 && !dir_.empty())
		(void)policy_.offer(r.triggers, r.frame, r.time_ms);
	if (policy_.due(r.time_ms)) {
		std::unique_lock<std::mutex> jobs(jobs_lock_);
		if (!dump_busy_ && writer_.joinable() && !quit_) {
			LARGE_INTEGER c0{}, c1{};
			QueryPerformanceCounter(&c0);
			Job job;
			job.dump = true;
			job.meta = policy_.take();
			job.count = snapshot(job.meta.trigger_ms - kFlightBeforeMs, dump_.data(), kFlightRing);
			dump_busy_ = true;
			jobs_.push_back(std::move(job));
			QueryPerformanceCounter(&c1);
			cost_.copy_ms_max = std::max(cost_.copy_ms_max,
				static_cast<double>(c1.QuadPart - c0.QuadPart) * 1000.0 / static_cast<double>(freq));
			jobs.unlock();
			wake_.notify_one();
		}
	}
	cur_ = FlightRecord{};
	LARGE_INTEGER end{};
	QueryPerformanceCounter(&end);
	const double us = static_cast<double>(end.QuadPart - start.QuadPart) * 1e6 / static_cast<double>(freq);
	++cost_.frames;
	cost_.total_us += us;
	cost_.max_us = std::max(cost_.max_us, us);
}

std::string FlightRecorder::csv_now(double ms)
{
	std::vector<FlightRecord> rows(kFlightRing);
	uint32_t n = 0;
	FlightNotes notes;
	{
		std::lock_guard<std::mutex> lock(lock_);
		const double since = ms > 0.0 && head_ != 0 ? ring_[(head_ - 1u) % kFlightRing].time_ms - ms : -1.0;
		n = snapshot(since, rows.data(), kFlightRing);
		notes = notes_;
	}
	std::string out = flight_csv_header();
	out.reserve(out.size() + static_cast<size_t>(n) * 200u);
	for (uint32_t i = 0; i < n; ++i)
		flight_csv_row(out, rows[i], notes);
	return out;
}

void FlightRecorder::post(std::function<void()> job)
{
	start_writer();
	{
		std::lock_guard<std::mutex> lock(jobs_lock_);
		if (quit_)
			return;
		Job j;
		j.work = std::move(job);
		jobs_.push_back(std::move(j));
	}
	wake_.notify_one();
}

void FlightRecorder::writer_main()
{
	SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(jobs_lock_);
			wake_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
			if (jobs_.empty())
				return;
			job = std::move(jobs_.front());
			jobs_.erase(jobs_.begin());
			working_ = true;
		}
		if (job.dump) {
			write_dump(job.meta, dump_.data(), job.count);
		} else if (job.work) {
			job.work();
		}
		{
			std::lock_guard<std::mutex> lock(jobs_lock_);
			if (job.dump)
				dump_busy_ = false;
			working_ = false;
		}
		wake_.notify_all();
	}
}

void FlightRecorder::write_dump(const FlightPolicy::Pending &meta, const FlightRecord *rows, uint32_t count, bool quiet)
{
	std::wstring dir;
	FlightNotes notes;
	if (quiet) {
		dir = dir_;
		notes = notes_;
	} else {
		std::lock_guard<std::mutex> lock(lock_);
		dir = dir_;
		notes = notes_;
	}
	if (dir.empty() || count == 0)
		return;
	std::string csv = flight_csv_header();
	csv.reserve(csv.size() + static_cast<size_t>(count) * 200u);
	for (uint32_t i = 0; i < count; ++i)
		flight_csv_row(csv, rows[i], notes);

	CreateDirectoryW(dir.c_str(), nullptr);
	std::vector<FlightFile> files;
	{
		WIN32_FIND_DATAW fd{};
		const HANDLE h = FindFirstFileW((dir + L"\\flight-*.csv").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
					files.push_back({ fd.cFileName,
						(static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow });
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
	}
	for (const std::wstring &old : flight_prune(files, meta.first, csv.size()))
		DeleteFileW((dir + L"\\" + old).c_str());
	{
		WIN32_FIND_DATAW fd{};
		const HANDLE h = FindFirstFileW((dir + L"\\flight-*.csv.part").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
	}

	SYSTEMTIME t{};
	GetLocalTime(&t);
	const std::wstring name = flight_file_name(t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
		t.wMilliseconds, meta.first);
	const std::wstring path = dir + L"\\" + name;
	const std::wstring part = path + L".part";
	bool ok = false;
	const HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f != INVALID_HANDLE_VALUE) {
		DWORD wrote = 0;
		ok = WriteFile(f, csv.data(), static_cast<DWORD>(csv.size()), &wrote, nullptr) && wrote == csv.size();
		CloseHandle(f);
		ok = ok && MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
		if (!ok)
			DeleteFileW(part.c_str());
	}
	if (quiet)
		return;
	Cost cost;
	{
		std::lock_guard<std::mutex> lock(lock_);
		if (ok)
			++cost_.dumps;
		cost = cost_;
	}
	{
		std::lock_guard<std::mutex> lock(jobs_lock_);
		written_ += ok ? 1u : 0u;
	}
	if (log_ == nullptr)
		return;
	std::wstring also;
	for (uint32_t k = 0; k < static_cast<uint32_t>(FlightTrigger::Count); ++k) {
		if ((meta.kinds & (1u << k)) != 0 && static_cast<FlightTrigger>(k) != meta.first) {
			const char *const n = flight_trigger_name(static_cast<FlightTrigger>(k));
			also += (also.empty() ? L"" : L", ") + std::wstring(n, n + std::strlen(n));
		}
	}
	wchar_t line[1024]{};
	const double span = count > 1 ? (rows[count - 1].time_ms - rows[0].time_ms) / 1000.0 : 0.0;
	if (ok)
		_snwprintf_s(line, _TRUNCATE, L"flight recorder: %hs at frame %llu, wrote %ls (%u frames, %.1f s%ls%ls; "
			L"recording costs %.2f us a frame on average, %.1f us at most)",
			flight_trigger_name(meta.first), static_cast<unsigned long long>(meta.frame), path.c_str(), count, span,
			also.empty() ? L"" : L"; also ", also.c_str(),
			cost.frames != 0 ? cost.total_us / static_cast<double>(cost.frames) : 0.0, cost.max_us);
	else
		_snwprintf_s(line, _TRUNCATE, L"flight recorder: %hs at frame %llu, could not write %ls (error %lu)",
			flight_trigger_name(meta.first), static_cast<unsigned long long>(meta.frame), path.c_str(),
			GetLastError());
	log_(line);
}

void FlightRecorder::shutdown()
{
	{
		std::lock_guard<std::mutex> lock(jobs_lock_);
		quit_ = true;
	}
	wake_.notify_all();
	if (writer_.joinable())
		writer_.join();
	FlightPolicy::Pending meta;
	uint32_t n = 0;
	{
		std::lock_guard<std::mutex> lock(lock_);
		if (stopped_)
			return;
		stopped_ = true;
		if (head_ == 0)
			return;
		meta.active = true;
		meta.first = FlightTrigger::Shutdown;
		meta.kinds = flight_bit(FlightTrigger::Shutdown);
		meta.frame = head_ - 1u;
		const double last = ring_[(head_ - 1u) % kFlightRing].time_ms;
		meta.trigger_ms = last;
		n = snapshot(last - kFlightBeforeMs, dump_.data(), kFlightRing);
	}
	write_dump(meta, dump_.data(), n);
}

void FlightRecorder::dump_at_exit() noexcept
{
	std::unique_lock<std::mutex> lock(lock_, std::try_to_lock);
	if (!lock.owns_lock() || stopped_ || head_ == 0)
		return;
	stopped_ = true;
	FlightPolicy::Pending meta;
	meta.active = true;
	meta.first = FlightTrigger::Shutdown;
	meta.kinds = flight_bit(FlightTrigger::Shutdown);
	meta.frame = head_ - 1u;
	meta.trigger_ms = ring_[(head_ - 1u) % kFlightRing].time_ms;
	const uint32_t n = snapshot(meta.trigger_ms - kFlightBeforeMs, dump_.data(), kFlightRing);
	lock.unlock();
	try {
		write_dump(meta, dump_.data(), n, true);
	} catch (...) {
	}
}

FlightRecorder::Cost FlightRecorder::cost() const
{
	std::lock_guard<std::mutex> lock(lock_);
	return cost_;
}

uint32_t FlightRecorder::dumps_written() const
{
	std::lock_guard<std::mutex> lock(jobs_lock_);
	return written_;
}

void FlightRecorder::drain()
{
	std::unique_lock<std::mutex> lock(jobs_lock_);
	wake_.wait_for(lock, std::chrono::seconds(30), [this] { return jobs_.empty() && !working_; });
}

}

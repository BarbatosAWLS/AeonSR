#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aeon_sr {

inline constexpr uint32_t kFlightRing = 8192u;
inline constexpr double kFlightBeforeMs = 20000.0;
inline constexpr double kFlightAfterMs = 2000.0;
inline constexpr double kFlightEveryMs = 60000.0;
inline constexpr uint32_t kFlightMaxFiles = 40u;
inline constexpr uint32_t kFlightMaxFilesPerKind = 5u;
inline constexpr uint64_t kFlightMaxBytes = 48ull << 20;

enum class FlightApi : uint8_t { Unknown, D3D9, D3D10, D3D11, D3D12, OpenGL, Vulkan, Count };
enum class FlightUpscaler : uint8_t { None, Dlss, DlssD3D11, Fsr, FsrD3D11, Xess, Vsr, Count };
enum class FlightJitter : uint8_t {
	Idle, Off, Drawn, Split, Ambiguous, Empty, Resampled, Paused, Withheld, NotDrawn, Count,
};
enum class FlightScene : uint8_t { Unknown, Depth, Size, Everything, Count };
enum class FlightHost : uint8_t { None, Answered, Timeout, Busy, Lost, Failed, Count };
enum class FlightBlocked : uint8_t { None, DeviceLost, NoBridge, NoBackBuffer, BridgeFailed, Count };
enum class FlightGameJitter : uint8_t { Unmeasured, No, Yes, Count };
enum class FlightTrigger : uint8_t {
	JitterFlap, JitterPause, LandingLow, GameJitter, HostSlow, Failure, FrameSpike, Shutdown, Report, Count,
};
inline constexpr uint16_t flight_bit(FlightTrigger t) noexcept { return static_cast<uint16_t>(1u << static_cast<uint32_t>(t)); }

inline constexpr uint8_t kFlightRestore = 1u;
inline constexpr uint8_t kFlightSplit = 2u;
inline constexpr uint8_t kFlightRegistered = 4u;
inline constexpr uint8_t kFlightAlone = 8u;
inline constexpr uint8_t kFlightHeld = 16u;

inline constexpr float kFlightNone = std::numeric_limits<float>::quiet_NaN();

struct FlightRecord {
	uint64_t frame = 0;
	uint64_t depth_id = 0;
	double time_ms = 0.0;
	float frame_ms = kFlightNone;
	float neural_ms = kFlightNone;
	float host_ms = kFlightNone;
	float drawn_x = kFlightNone, drawn_y = kFlightNone;
	float told_x = kFlightNone, told_y = kFlightNone;
	float landing_x = kFlightNone, landing_y = kFlightNone;
	float game_depth_x = kFlightNone, game_depth_y = kFlightNone;
	float game_color_x = kFlightNone, game_color_y = kFlightNone;
	float game_rms = kFlightNone;
	uint16_t width = 0, height = 0;
	uint16_t render_w = 0, render_h = 0;
	uint16_t depth_w = 0, depth_h = 0;
	uint16_t moved = 0, plain = 0;
	uint16_t drawn_index = 0;
	uint16_t triggers = 0;
	FlightApi api = FlightApi::Unknown;
	FlightUpscaler upscaler = FlightUpscaler::None;
	uint8_t status = 0;
	int8_t ran = -1;
	int8_t neural = 0;
	FlightJitter jitter = FlightJitter::Idle;
	uint8_t plan = 0xFFu;
	uint8_t note = 0;
	uint8_t pause = 0;
	FlightScene scene = FlightScene::Unknown;
	uint8_t hud = 0;
	FlightHost host = FlightHost::None;
	FlightGameJitter game_jitter = FlightGameJitter::Unmeasured;
	FlightBlocked blocked = FlightBlocked::None;
};

const char *flight_api_name(FlightApi a) noexcept;
const char *flight_upscaler_name(FlightUpscaler u) noexcept;
const char *flight_jitter_name(FlightJitter j) noexcept;
const char *flight_scene_name(FlightScene s) noexcept;
const char *flight_host_name(FlightHost h) noexcept;
const char *flight_blocked_name(FlightBlocked b) noexcept;
const char *flight_game_jitter_name(FlightGameJitter g) noexcept;
const char *flight_trigger_name(FlightTrigger t) noexcept;
const char *flight_status_name(uint8_t status) noexcept;
const char *flight_plan_name(uint8_t plan) noexcept;

class FlightNotes {
public:
	static constexpr uint32_t kMax = 64u;
	static constexpr uint32_t kChars = 112u;
	static constexpr uint8_t kFull = 0xFFu;
	uint8_t id(const char *text) noexcept;
	const char *text(uint8_t id) const noexcept;
	uint32_t size() const noexcept { return count_; }

private:
	const char *ptr_[kMax]{};
	char text_[kMax][kChars]{};
	uint32_t count_ = 1u;
};

class FlightDetector {
public:
	static constexpr uint32_t kFlapChanges = 6u;
	static constexpr double kFlapWindowMs = 2000.0;
	static constexpr float kLandingLow = 0.5f;
	static constexpr float kHostSlowMs = 50.0f;
	static constexpr float kSpikeRatio = 2.5f;
	static constexpr float kSpikeAboveMs = 20.0f;
	static constexpr uint32_t kSpikeWarmup = 120u;

	uint16_t detect(const FlightRecord &r) noexcept;
	float mean_frame_ms() const noexcept { return mean_ms_; }

private:
	static constexpr uint32_t kChangeSlots = 16u;
	double changes_[kChangeSlots]{};
	uint32_t change_next_ = 0;
	int flap_class_ = -1;
	bool paused_ = false;
	FlightGameJitter game_ = FlightGameJitter::Unmeasured;
	bool failed_ = false;
	bool neural_failed_ = false;
	FlightBlocked blocked_ = FlightBlocked::None;
	float mean_ms_ = 0.0f;
	uint32_t timed_ = 0;
};

class FlightPolicy {
public:
	struct Pending {
		bool active = false;
		FlightTrigger first = FlightTrigger::Count;
		uint16_t kinds = 0;
		uint64_t frame = 0;
		double trigger_ms = 0.0;
		double due_ms = 0.0;
	};
	bool offer(uint16_t triggers, uint64_t frame, double t_ms) noexcept;
	bool due(double t_ms) const noexcept { return pending_.active && t_ms >= pending_.due_ms; }
	const Pending &pending() const noexcept { return pending_; }
	Pending take() noexcept;

private:
	Pending pending_;
	double last_ms_[static_cast<uint32_t>(FlightTrigger::Count)]{};
	bool seen_[static_cast<uint32_t>(FlightTrigger::Count)]{};
};

struct FlightFile {
	std::wstring name;
	uint64_t bytes = 0;
};
std::wstring flight_file_name(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute,
	uint32_t second, uint32_t ms, FlightTrigger kind);
std::wstring flight_file_kind(const std::wstring &name);
std::vector<std::wstring> flight_prune(std::vector<FlightFile> files, FlightTrigger kind, uint64_t incoming,
	uint32_t max_files = kFlightMaxFiles, uint32_t max_per_kind = kFlightMaxFilesPerKind,
	uint64_t max_bytes = kFlightMaxBytes);

std::string flight_csv_header();
void flight_csv_row(std::string &out, const FlightRecord &r, const FlightNotes &notes);

struct FlightFault {
	static constexpr uint32_t kMax = 8u;
	FlightTrigger kind[kMax]{};
	uint64_t frame[kMax]{};
	uint32_t count = 0;
	uint16_t at(uint64_t frame) const noexcept;
};
bool parse_flight_fault(const std::string &text, FlightFault *out);
bool flight_trigger_of(const std::string &name, FlightTrigger *out) noexcept;

class FlightRecorder {
public:
	FlightRecorder();
	~FlightRecorder();
	FlightRecorder(const FlightRecorder &) = delete;
	FlightRecorder &operator=(const FlightRecorder &) = delete;

	void set_dir(const std::wstring &dir);
	void set_log(void (*log)(const std::wstring &line)) noexcept { log_ = log; }

	FlightRecord &cur() noexcept { return cur_; }
	void commit(int64_t qpc, int64_t qpf, uint16_t forced, const char *note, const char *pause) noexcept;
	std::string csv_now(double ms);
	void post(std::function<void()> job);
	void shutdown();
	void dump_at_exit() noexcept;

	struct Cost {
		uint64_t frames = 0;
		double total_us = 0.0, max_us = 0.0;
		double copy_ms_max = 0.0;
		uint32_t dumps = 0;
	};
	Cost cost() const;
	uint32_t dumps_written() const;
	void drain();

private:
	struct Job {
		bool dump = false;
		FlightPolicy::Pending meta;
		uint32_t count = 0;
		std::function<void()> work;
	};
	void writer_main();
	void write_dump(const FlightPolicy::Pending &meta, const FlightRecord *rows, uint32_t count, bool quiet = false);
	uint32_t snapshot(double since_ms, FlightRecord *out, uint32_t room) const noexcept;
	void start_writer();

	FlightRecord cur_{};
	FlightDetector detector_;
	FlightPolicy policy_;
	FlightNotes notes_;
	std::vector<FlightRecord> ring_;
	std::vector<FlightRecord> dump_;
	uint64_t head_ = 0;
	int64_t first_qpc_ = 0, last_qpc_ = 0;
	mutable std::mutex lock_;
	Cost cost_;

	std::wstring dir_;
	void (*log_)(const std::wstring &) = nullptr;
	mutable std::mutex jobs_lock_;
	std::condition_variable wake_;
	std::vector<Job> jobs_;
	bool dump_busy_ = false;
	bool working_ = false;
	bool quit_ = false;
	bool stopped_ = false;
	uint32_t written_ = 0;
	std::thread writer_;
};

}

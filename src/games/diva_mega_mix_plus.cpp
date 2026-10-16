#include "aeon_sr/games/diva_mega_mix_plus.hpp"
#include "aeon_sr/games/game_integration.hpp"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstring>

namespace aeon_sr::games {

const CompatEntry &diva_mega_mix_plus_entry()
{
	static const CompatEntry e{
		"diva_mega_mix_plus",
		"Hatsune Miku: Project DIVA Mega Mix+",
		L"DivaMegaMix.exe",
		"in-house (SEGA, Project DIVA Arcade engine)",
		"Direct3D 11",
		"the game's Image Quality value, answered by AeonSR every frame (its getter hooked)",
		"the game's own stretch of the scene's corner, before lyrics, notes and interface",
		true,
		"AeonSR's estimator (the game writes none)",
		"0.33-1.0 asked; 0.5-1.0 is the game's own range, below 0.5 untested by the game",
		"working (Direct3D 11)",
		"Steam, DivaMegaMix.exe TimeDateStamp 0x635B31AB (2022-10-28)",
	};
	return e;
}

namespace {

constexpr const char *kGetter =
	"8B 15 ?? ?? ?? ?? 44 8B CA 41 C1 E9 19 44 8B C2 41 80 E1 01 74 06 41 C1 E8 1B EB 15";
constexpr const char *kRendererCall = "E8 ?? ?? ?? ?? 44 8B 86 D0 0F 00 00 0F 28 D8 F3 0F 59 5D 70";
constexpr const char *kRenderGlobal = "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC E9 EB FF FF FF";
constexpr const char *kAppliedRead = "48 8B 83 ?? ?? ?? ?? 8B 8B ?? ?? ?? ?? F3 0F 10 88 E0 04 00 00 E8";
constexpr uint32_t kAppliedOffset = 0x4E0;

using Getter = float (*)();
Getter g_original = nullptr;
std::atomic<float> g_answer{ 0.0f };
const volatile uint32_t *g_flags = nullptr;
const volatile int32_t *g_counter = nullptr;

bool game_forces() noexcept
{
	if (g_flags == nullptr || g_counter == nullptr)
		return false;
	const uint32_t f = *g_flags;
	const uint32_t sel = (f >> 25) & 1u ? f >> 27 : (f >> 24) & 1u ? f >> 15 : f >> 13;
	return (sel & 1u) != 0 && *g_counter > 0;
}

float getter_detour()
{
	const float s = g_answer.load(std::memory_order_relaxed);
	return s > 0.0f && !game_forces() ? s : g_original();
}

float read_applied(const uint8_t *const *render_global, uint32_t data_offset) noexcept
{
	__try {
		const uint8_t *const render = *render_global;
		if (render == nullptr)
			return 0.0f;
		const uint8_t *const data = *reinterpret_cast<const uint8_t *const *>(render + data_offset);
		if (data == nullptr)
			return 0.0f;
		const float v = *reinterpret_cast<const float *>(data + kAppliedOffset);
		return v > 0.0f && v <= 4.0f ? v : 0.0f;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0.0f;
	}
}

size_t find_once(const ModuleImage &image, const char *pattern, const uint8_t **found)
{
	BytePattern p;
	return parse_pattern(pattern, &p) ? find_in_code(image, p, found, 1) : 0;
}

class DivaMegaMixPlus final : public GameIntegration {
public:
	const CompatEntry &entry() const override { return diva_mega_mix_plus_entry(); }

	bool attach(std::wstring *why) override
	{
		if (attached_)
			return true;
		ModuleImage img;
		if (!module_image(GetModuleHandleW(nullptr), &img)) {
			*why = L"the game's executable could not be read";
			return false;
		}
		timestamp_ = img.timestamp;
		DivaLocations at;
		if (!diva_locate(img, &at, why))
			return false;
		const MH_STATUS up = MH_Initialize();
		if (up != MH_OK && up != MH_ERROR_ALREADY_INITIALIZED) {
			*why = L"the hook library would not start";
			return false;
		}
		void *const target = const_cast<uint8_t *>(at.getter);
		void *original = nullptr;
		if (MH_CreateHook(target, reinterpret_cast<void *>(&getter_detour), &original) != MH_OK || original == nullptr) {
			*why = L"the Image Quality function could not be hooked";
			return false;
		}
		g_original = reinterpret_cast<Getter>(original);
		g_flags = reinterpret_cast<const volatile uint32_t *>(at.flags);
		g_counter = reinterpret_cast<const volatile int32_t *>(at.counter);
		if (MH_EnableHook(target) != MH_OK) {
			MH_RemoveHook(target);
			g_original = nullptr;
			*why = L"the Image Quality function could not be hooked";
			return false;
		}
		at_ = at;
		attached_ = true;
		return true;
	}

	void detach() override
	{
		g_answer.store(0.0f);
		if (at_.getter != nullptr)
			MH_DisableHook(const_cast<uint8_t *>(at_.getter));
		at_ = DivaLocations{};
		attached_ = false;
	}

	bool attached() const override { return attached_; }

	std::wstring note() const override
	{
		if (!attached_)
			return L"not attached";
		return timestamp_ == kDivaTestedTimestamp
			? L"AeonSR sets the game's Image Quality (the build this was made for)"
			: L"AeonSR sets the game's Image Quality (a build this was not made for: what the game applies is read "
			  L"back every frame)";
	}

	void set_render_scale(float scale) noexcept override
	{
		const float s = scale > 0.0f ? (scale < 0.25f ? 0.25f : scale > 1.0f ? 1.0f : scale) : 0.0f;
		g_answer.store(attached_ ? s : 0.0f, std::memory_order_relaxed);
	}

	float requested_render_scale() const noexcept override { return g_answer.load(std::memory_order_relaxed); }

	float applied_render_scale() const noexcept override
	{
		return attached_ && at_.render_global != nullptr
			? read_applied(reinterpret_cast<const uint8_t *const *>(at_.render_global), at_.data_offset) : 0.0f;
	}

	bool replaces_stretch() const noexcept override { return true; }
	bool routes_full_size() const noexcept override { return true; }
	bool reads_before_game_aa() const noexcept override { return true; }

private:
	bool attached_ = false;
	DivaLocations at_;
	uint32_t timestamp_ = 0;
};

}

bool diva_locate(const ModuleImage &image, DivaLocations *out, std::wstring *why)
{
	*out = DivaLocations{};
	const uint8_t *getter = nullptr, *call = nullptr, *global_fn = nullptr, *applied[2]{};
	size_t n = find_once(image, kGetter, &getter);
	if (n != 1) {
		*why = L"the Image Quality function was found " + std::to_wstring(n) + L" times (one expected): a build of the "
			L"game this was not made for";
		return false;
	}
	n = find_once(image, kRendererCall, &call);
	if (n != 1) {
		*why = L"the renderer's call of the Image Quality function was found " + std::to_wstring(n) + L" times";
		return false;
	}
	const uint8_t *const thunk = rel32_target(call + 1, 4);
	if (thunk < image.base || thunk + 5 > image.base + image.size || thunk[0] != 0xE9 ||
		rel32_target(thunk + 1, 4) != getter) {
		*why = L"the renderer does not call the Image Quality function that was found";
		return false;
	}
	n = find_once(image, kRenderGlobal, &global_fn);
	if (n != 1) {
		*why = L"the renderer's own record was found " + std::to_wstring(n) + L" times";
		return false;
	}
	const uint8_t *const global = rel32_target(global_fn + 3, 4);
	if (global < image.base || global + sizeof(void *) > image.base + image.size) {
		*why = L"the renderer's own record lies outside the game";
		return false;
	}
	const uint8_t *const flags = rel32_target(getter + 2, 4);
	const uint8_t *const counter =
		getter[0x37] == 0x83 && getter[0x38] == 0x3D && getter[0x3D] == 0x00 && getter[0x3E] == 0x7E
		? rel32_target(getter + 0x39, 5) : nullptr;
	if (counter == nullptr || flags < image.base || flags + 4 > image.base + image.size || counter < image.base ||
		counter + 4 > image.base + image.size) {
		*why = L"the Image Quality function does not test what it was read to test";
		return false;
	}
	BytePattern p;
	n = parse_pattern(kAppliedRead, &p) ? find_in_code(image, p, applied, 2) : 0;
	if (n < 1 || n > 2) {
		*why = L"where the renderer keeps the scale it applied was found " + std::to_wstring(n) + L" times";
		return false;
	}
	uint32_t offset = 0;
	std::memcpy(&offset, applied[0] + 3, 4);
	if (n == 2) {
		uint32_t other = 0;
		std::memcpy(&other, applied[1] + 3, 4);
		if (other != offset) {
			*why = L"the renderer's data is reached two different ways";
			return false;
		}
	}
	out->getter = getter;
	out->render_global = global;
	out->data_offset = offset;
	out->flags = flags;
	out->counter = counter;
	return true;
}

GameIntegration *make_diva_mega_mix_plus()
{
	static DivaMegaMixPlus g;
	return &g;
}

}

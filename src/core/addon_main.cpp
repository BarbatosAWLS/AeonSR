#include "aeon_sr/core/app.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/core/diag_report.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/game_render_scale.hpp"
#include "aeon_sr/core/pipeline_trace.hpp"
#include "aeon_sr/upscalers/native_dlss.hpp"
#include "aeon_sr/core/register_compat.hpp"
#include "aeon_sr/jitter/scene_jitter_hooks.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/interop/d3d9ex_upgrade.hpp"
#if AEONSR_FRAME_GENERATION
#include "aeon_sr/framegen/frame_generation.hpp"
#include "aeon_sr/framegen/overlay_presenter.hpp"
#include "aeon_sr/framegen/present_sync.hpp"
#endif

#include <atomic>
#include <cwchar>
#include <cwctype>

using namespace aeon_sr;

namespace {

std::wstring d3d9ex_attempts(const d3d9ex::Result &r)
{
	std::wstring s;
	for (int i = 0; i < r.attempt_count; ++i) {
		const d3d9ex::Attempt &a = r.attempts[i];
		const wchar_t *const format = a.format == D3DFMT_X8R8G8B8 ? L"X8R8G8B8"
			: a.format == D3DFMT_A8R8G8B8 ? L"A8R8G8B8" : L"other format";
		const wchar_t *const order = a.scanline == D3DSCANLINEORDERING_PROGRESSIVE ? L"progressive"
			: a.scanline == D3DSCANLINEORDERING_INTERLACED ? L"interlaced" : L"no scan-line order";
		wchar_t one[160];
		_snwprintf_s(one, _TRUNCATE, L"%ls%ux%u %u Hz %ls (%u) %ls 0x%08lX", i == 0 ? L"" : L"; ", a.width, a.height,
			a.refresh, format, static_cast<unsigned>(a.format), order, static_cast<unsigned long>(a.hr));
		s += one;
	}
	return s;
}

bool from_own_window() noexcept
{
#if AEONSR_FRAME_GENERATION
	return overlay_on_own_thread();
#else
	return false;
#endif
}

bool on_create_device(reshade::api::device_api api, uint32_t &api_version)
{
#if AEONSR_FRAME_GENERATION
	if (overlay_creating_device())
		return false;
#endif
	scene_jitter_device_coming(api);
	if (api != reshade::api::device_api::d3d9)
		return false;
	d3d9ex::disarm();
	if (api_version != 0x9000)
		return false;
	if (!d3d9ex::arm())
		return false;
	api_version = 0x9100;
	return true;
}

void on_init_device(reshade::api::device *device)
{
#if AEONSR_FRAME_GENERATION
	if (overlay_creating_device()) {
		if (device != nullptr)
			overlay_note_own_device(device->get_native());
		return;
	}
#endif
	if (device != nullptr && device->get_api() == reshade::api::device_api::d3d9) {
		d3d9ex::disarm();
		const d3d9ex::Result r = d3d9ex::take_result();
		wchar_t hr[16];
		_snwprintf_s(hr, _TRUNCATE, L"0x%08lX", static_cast<unsigned long>(r.ex_hr));
		if (r.outcome == d3d9ex::Outcome::Upgraded)
			diag_info("interop", std::wstring(L"the game's Direct3D 9 device was made Direct3D 9Ex, so its frames "
				L"reach the upscaler on the graphics card") + (r.mode_made ? L" (fullscreen: the display mode was "
				L"built from the game's own settings" : L"") + (r.refresh_filled != 0 ? L", at " +
				std::to_wstring(r.refresh_filled) + L" Hz where it asked for no refresh rate" : L"") +
				(r.mode_made ? L")" : L"") + (r.attempt_count > 1 ? L"; display modes asked: " + d3d9ex_attempts(r) : L""));
		else if (r.outcome == d3d9ex::Outcome::FellBack)
			diag_warn("interop", std::wstring(L"Direct3D 9Ex refused this game's device (") + hr + L"), so it "
				L"was made the plain Direct3D 9 device the game asked for, and its frames travel through system memory" +
				(r.attempt_count > 0 ? L"; display modes asked: " + d3d9ex_attempts(r) : L""));
	}
	if (auto *a = app())
		a->on_init_device(device);
}

void on_destroy_device(reshade::api::device *device)
{
#if AEONSR_FRAME_GENERATION
	if (device != nullptr && overlay_forget_own_device(device->get_native()))
		return;
#endif
	if (auto *a = app())
		a->on_destroy_device(device);
}

void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
	if (from_own_window())
		return;
	if (auto *a = app())
		a->on_init_swapchain(swapchain, resize);
}

bool fill_d3d9_refresh(reshade::api::device_api api, reshade::api::swapchain_desc &desc)
{
	if (api != reshade::api::device_api::d3d9 || !desc.fullscreen_state || desc.fullscreen_refresh_rate != 0.0f)
		return false;
	const reshade::api::format f = desc.back_buffer.texture.format;
	const D3DFORMAT format = f == reshade::api::format::b8g8r8a8_unorm ? D3DFMT_A8R8G8B8
		: f == reshade::api::format::b8g8r8x8_unorm ? D3DFMT_X8R8G8B8 : D3DFMT_UNKNOWN;
	const UINT rate = d3d9ex::refresh_for_reset(desc.back_buffer.texture.width, desc.back_buffer.texture.height,
		format);
	if (rate == 0)
		return false;
	desc.fullscreen_refresh_rate = static_cast<float>(rate);
	diag_logf(DiagLevel::Info, "interop", L"the game's fullscreen Direct3D 9Ex device asked for no refresh rate "
		L"again; %u Hz, one of the display's own, which Direct3D 9Ex takes where it refuses none", rate);
	return true;
}

bool on_create_swapchain(reshade::api::device_api api, reshade::api::swapchain_desc &desc, void *)
{
	if (from_own_window())
		return false;
#if AEONSR_FRAME_GENERATION
	if (api == reshade::api::device_api::vulkan)
		FrameGeneration::note_vulkan_present_mode(desc.present_mode);
#endif
	const bool refresh = fill_d3d9_refresh(api, desc);
	if (!request_swapchain_usage(api, desc))
		return refresh;
	diag_info("swapchain", L"asked for transfer usage on the Vulkan swap chain, so the frame can be "
		L"carried to the upscalers and back");
	return true;
}

void on_destroy_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
	if (from_own_window())
		return;
	if (auto *a = app())
		a->on_destroy_swapchain(swapchain, resize);
}

void on_begin_effects(
	reshade::api::effect_runtime *runtime,
	reshade::api::command_list *cmd_list,
	reshade::api::resource_view rtv,
	reshade::api::resource_view rtv_srgb)
{
	if (from_own_window())
		return;
	if (auto *a = app())
		a->on_begin_effects(runtime, cmd_list, rtv, rtv_srgb);
}

void on_finish_effects(
	reshade::api::effect_runtime *runtime,
	reshade::api::command_list *cmd_list,
	reshade::api::resource_view rtv,
	reshade::api::resource_view rtv_srgb)
{
	if (from_own_window())
		return;
	if (auto *a = app())
		a->on_finish_effects(runtime, cmd_list, rtv, rtv_srgb);
}

void on_reloaded_effects(reshade::api::effect_runtime *runtime)
{
	if (from_own_window())
		return;
	if (auto *a = app())
		a->on_reloaded_effects(runtime);
}

void on_present(reshade::api::effect_runtime *runtime)
{
	if (from_own_window())
		return;
	if (auto *a = app())
		a->on_present(runtime);
}

void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
	if (from_own_window())
		return;
	if (auto *a = app())
		a->on_init_effect_runtime(runtime);
}

void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
{
	if (from_own_window())
		return;
	if (auto *a = app())
		a->on_destroy_effect_runtime(runtime);
}

void on_game_present(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain,
	const reshade::api::rect *, const reshade::api::rect *, uint32_t, const reshade::api::rect *)
{
	if (from_own_window())
		return;
	if (auto *a = app())
		a->on_game_present(queue, swapchain);
}

void on_finish_present(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain)
{
	if (from_own_window())
		return;
	if (auto *a = app())
		a->on_finish_present(queue, swapchain);
}

void draw_main_overlay(reshade::api::effect_runtime *runtime)
{
	if (auto *a = app())
		a->draw_overlay(runtime);
}

void draw_osd(reshade::api::effect_runtime *runtime)
{
	if (auto *a = app())
		a->draw_osd(runtime);
}

void mirror_to_host_log(DiagLevel level, const char *message)
{
	reshade::log::message(level == DiagLevel::Error
		? reshade::log::level::error : reshade::log::level::warning, message);
}

}

extern "C" __declspec(dllexport) const char *NAME = "Aeon SR";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"Multi-upscaler AA (DLSS / FSR / XeSS) with built-in optical flow, "
	"on Direct3D 8, 9, 10, 11 and 12, OpenGL and Vulkan.";

namespace {

PVOID g_driver_fault_handler = nullptr;
std::atomic<bool> g_driver_fault_logged{ false };

bool driver_module(const wchar_t *path) noexcept
{
	static const wchar_t *const kModules[] = { L"nvwgf2um", L"d3d12core", L"d3d12.dll", L"nvngx",
		L"nvgpucomp", L"amdxc", L"igd12" };
	wchar_t lower[MAX_PATH]{};
	for (size_t i = 0; path[i] != L'\0' && i + 1 < MAX_PATH; ++i)
		lower[i] = static_cast<wchar_t>(towlower(path[i]));
	for (const wchar_t *m : kModules) {
		if (wcsstr(lower, m) != nullptr)
			return true;
	}
	return false;
}

LONG CALLBACK driver_fault_handler(EXCEPTION_POINTERS *ep)
{
	if (ep == nullptr || ep->ExceptionRecord == nullptr ||
		ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;
	const void *const at = ep->ExceptionRecord->ExceptionAddress;
	HMODULE module = nullptr;
	wchar_t path[MAX_PATH]{};
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<LPCWSTR>(at), &module) ||
		GetModuleFileNameW(module, path, MAX_PATH) == 0 || !driver_module(path))
		return EXCEPTION_CONTINUE_SEARCH;
	if (g_driver_fault_logged.exchange(true))
		return EXCEPTION_CONTINUE_SEARCH;
	const wchar_t *name = wcsrchr(path, L'\\');
	diag_logf(DiagLevel::Error, "crash",
		L"access violation (first chance) in %s at +0x%llx on thread %lu, %s address 0x%llx; the "
		L"engine's last transitions, oldest first:",
		name != nullptr ? name + 1 : path,
		static_cast<unsigned long long>(static_cast<const char *>(at) - reinterpret_cast<const char *>(module)),
		GetCurrentThreadId(),
		ep->ExceptionRecord->NumberParameters > 0 && ep->ExceptionRecord->ExceptionInformation[0] == 1 ? L"writing"
			: L"reading",
		static_cast<unsigned long long>(ep->ExceptionRecord->NumberParameters > 1
			? ep->ExceptionRecord->ExceptionInformation[1] : 0));
	engine_journal_dump();
	return EXCEPTION_CONTINUE_SEARCH;
}

}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
	diag_open_log(join_path(module_directory(addon_module), L"AeonSR.log"));
	g_driver_fault_handler = AddVectoredExceptionHandler(0, driver_fault_handler);
	diag_collect_environment_local(addon_module);
	diag_log_environment();

	if (!register_addon_compat(addon_module, reshade_module)) {
		diag_error("aeonsr",
			L"ReShade refused to register this add-on. It is older than the add-on "
			L"needs, or it is a build without add-on support. Install the ReShade "
			L"version with full add-on support.");
		return false;
	}
	diag_set_sink(mirror_to_host_log);

	create_app(addon_module);

	reshade::register_event<reshade::addon_event::create_device>(on_create_device);
	reshade::register_event<reshade::addon_event::init_device>(on_init_device);
	reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
	reshade::register_event<reshade::addon_event::create_swapchain>(on_create_swapchain);
	reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
	reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
	reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
	reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
	reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
	reshade::register_event<reshade::addon_event::reshade_present>(on_present);
	reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
	reshade::register_event<reshade::addon_event::present>(on_game_present);
	reshade::register_event<reshade::addon_event::finish_present>(on_finish_present);
	register_scene_jitter_hooks();
	if (App *const a = app(); a != nullptr && a->settings().pipeline_trace) {
		const std::string &d = a->settings().pipeline_trace_dir;
		std::wstring dir(d.begin(), d.end());
		while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
			dir.pop_back();
		if (dir.empty())
			dir = join_path(module_directory(addon_module), L"captures");
		register_pipeline_trace(a->settings().pipeline_trace_key, dir);
	}

	reshade::register_overlay("Aeon SR", draw_main_overlay);
	reshade::register_overlay("OSD", draw_osd);

	return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
	if (g_driver_fault_handler != nullptr) {
		RemoveVectoredExceptionHandler(g_driver_fault_handler);
		g_driver_fault_handler = nullptr;
	}
	d3d9ex::disarm();
	unregister_pipeline_trace();
	unregister_game_render_scale();
#if AEONSR_FRAME_GENERATION
	present_sync::unhook();
#endif
	destroy_app();
	diag_set_sink(nullptr);
	diag_info("aeonsr", L"add-on unloaded");
	diag_close_log();
	reshade::unregister_addon(addon_module, reshade_module);
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
		if (aeon_sr::App *const a = aeon_sr::app())
			a->at_process_exit();
	}
	return TRUE;
}

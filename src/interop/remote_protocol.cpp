#include "aeon_sr/interop/remote_protocol.hpp"

#include <cwchar>

namespace aeon_sr::remote {

const char *remote_step_label(RemoteStep step) noexcept
{
	switch (step) {
	case RemoteStep::Ok: return "ok";
	case RemoteStep::NoHost: return "the engine host is not running";
	case RemoteStep::Handshake: return "the engine host is a different build";
	case RemoteStep::Timeout: return "the engine host did not answer in time";
	case RemoteStep::OpenPlanes: return "the engine host could not open the game's textures";
	case RemoteStep::Engine: return "the engine host has no Direct3D 12 device";
	case RemoteStep::Upscaler: return "the upscaler";
	case RemoteStep::Lost: return "the engine host exited";
	case RemoteStep::Share: return "this frame could not be made shareable";
	}
	return "unknown";
}

const char *plane_role_label(PlaneRole role) noexcept
{
	switch (role) {
	case PlaneRole::Colour: return "colour";
	case PlaneRole::Depth: return "depth";
	case PlaneRole::Motion: return "motion";
	case PlaneRole::Guide: return "guide";
	case PlaneRole::Count: break;
	}
	return "unknown";
}

void session_names(uint32_t game_pid, SessionNames &out) noexcept
{
	swprintf(out.block, kNameChars, L"Local\\AeonSR_%u_block", game_pid);
	swprintf(out.request, kNameChars, L"Local\\AeonSR_%u_request", game_pid);
	swprintf(out.response, kNameChars, L"Local\\AeonSR_%u_response", game_pid);
}

void object_name(uint32_t game_pid, const wchar_t *what, uint64_t generation,
	wchar_t *out, uint32_t out_chars) noexcept
{
	if (out == nullptr || out_chars == 0)
		return;
	out[0] = L'\0';
	if (what == nullptr)
		return;
	swprintf(out, out_chars, L"Local\\AeonSR_%u_%s_%llu", game_pid, what,
		static_cast<unsigned long long>(generation));
}

}

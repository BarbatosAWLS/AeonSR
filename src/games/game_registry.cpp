#include "aeon_sr/games/game_integration.hpp"
#include "aeon_sr/core/settings.hpp"

#include <windows.h>

#include <cwchar>

namespace aeon_sr::games {

const CompatEntry &diva_mega_mix_plus_entry();
GameIntegration *make_diva_mega_mix_plus();

namespace {

struct Listed {
	const CompatEntry &(*entry)();
	GameIntegration *(*make)();
};

const Listed kGames[] = {
	{ &diva_mega_mix_plus_entry, &make_diva_mega_mix_plus },
};

}

const CompatEntry *compatibility_list(size_t *count)
{
	static CompatEntry list[sizeof(kGames) / sizeof(kGames[0])] = {};
	static bool made = false;
	if (!made) {
		for (size_t i = 0; i < sizeof(kGames) / sizeof(kGames[0]); ++i)
			list[i] = kGames[i].entry();
		made = true;
	}
	*count = sizeof(kGames) / sizeof(kGames[0]);
	return list;
}

const CompatEntry *find_entry(const wchar_t *exe_name)
{
	if (exe_name == nullptr)
		return nullptr;
	for (const Listed &g : kGames) {
		const CompatEntry &e = g.entry();
		if (_wcsicmp(e.exe, exe_name) == 0)
			return &e;
	}
	return nullptr;
}

GameIntegration *integration_for_process()
{
	wchar_t path[MAX_PATH]{};
	if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
		return nullptr;
	const wchar_t *name = wcsrchr(path, L'\\');
	name = name != nullptr ? name + 1 : path;
	for (const Listed &g : kGames) {
		if (_wcsicmp(g.entry().exe, name) == 0)
			return g.make();
	}
	return nullptr;
}

float render_scale_for_mode(unsigned int upscale_mode) noexcept
{
	return vsr_scale_for_mode(static_cast<UpscaleMode>(upscale_mode));
}

}

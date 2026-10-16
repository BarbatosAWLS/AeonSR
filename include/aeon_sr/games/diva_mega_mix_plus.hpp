#pragma once

#include "aeon_sr/games/signature_scan.hpp"

#include <cstdint>
#include <string>

namespace aeon_sr::games {

struct DivaLocations {
	const uint8_t *getter = nullptr;
	const uint8_t *render_global = nullptr;
	uint32_t data_offset = 0;
	const uint8_t *flags = nullptr;
	const uint8_t *counter = nullptr;
};

inline constexpr uint32_t kDivaTestedTimestamp = 0x635B31ABu;

bool diva_locate(const ModuleImage &image, DivaLocations *out, std::wstring *why);

}

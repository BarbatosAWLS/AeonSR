#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aeon_sr::games {

struct BytePattern {
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> known;
};

bool parse_pattern(const char *text, BytePattern *out);

size_t find_pattern(const uint8_t *data, size_t size, const BytePattern &p, size_t *offsets, size_t limit);

struct ModuleImage {
	const uint8_t *base = nullptr;
	size_t size = 0;
	uint32_t timestamp = 0;
	uint32_t size_of_image = 0;
	std::vector<std::pair<size_t, size_t>> code;
};

bool module_image(const void *base, ModuleImage *out);

size_t find_in_code(const ModuleImage &image, const BytePattern &p, const uint8_t **found, size_t limit);

inline const uint8_t *rel32_target(const uint8_t *at, size_t instruction_end)
{
	int32_t rel = 0;
	for (int i = 0; i < 4; ++i)
		rel |= static_cast<int32_t>(static_cast<uint32_t>(at[i]) << (8 * i));
	return at + instruction_end + rel;
}

}

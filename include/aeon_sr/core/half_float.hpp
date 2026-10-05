#pragma once

#include <cstdint>
#include <cstring>

namespace aeon_sr {

inline float half_to_float(uint16_t h) noexcept
{
	const uint32_t sign = (h >> 15) & 1u;
	const uint32_t exp = (h >> 10) & 0x1Fu;
	const uint32_t mant = h & 0x3FFu;

	uint32_t f;
	if (exp == 0) {

		if (mant == 0) {
			f = sign << 31;
		} else {
			uint32_t e = 0;
			uint32_t m = mant;
			while ((m & 0x400u) == 0) { m <<= 1; ++e; }
			m &= 0x3FFu;
			const uint32_t fexp = (127u - 14u - e) << 23;
			f = (sign << 31) | fexp | (m << 13);
		}
	} else if (exp == 31) {
		f = (sign << 31) | 0x7F800000u | (mant << 13);
	} else {
		f = (sign << 31) | ((exp + (127u - 15u)) << 23) | (mant << 13);
	}

	float out;
	static_assert(sizeof(out) == sizeof(f), "");
	memcpy(&out, &f, sizeof(out));
	return out;
}

inline uint16_t float_to_half(float value) noexcept
{
	uint32_t f;
	static_assert(sizeof(f) == sizeof(value), "");
	memcpy(&f, &value, sizeof(f));
	const uint32_t sign = (f >> 16) & 0x8000u;
	const int32_t exp = static_cast<int32_t>((f >> 23) & 0xFFu) - 127 + 15;
	const uint32_t mant = f & 0x7FFFFFu;

	if (((f >> 23) & 0xFFu) == 0xFFu)
		return static_cast<uint16_t>(sign | 0x7C00u | (mant != 0 ? 0x200u : 0u));
	if (exp >= 0x1F)
		return static_cast<uint16_t>(sign | 0x7C00u);
	if (exp <= 0) {

		if (exp < -10)
			return static_cast<uint16_t>(sign);
		const uint32_t m = mant | 0x800000u;
		const uint32_t shift = static_cast<uint32_t>(14 - exp);
		uint32_t h = m >> shift;
		const uint32_t rest = m & ((1u << shift) - 1u);
		const uint32_t half_way = 1u << (shift - 1u);
		if (rest > half_way || (rest == half_way && (h & 1u) != 0u))
			++h;
		return static_cast<uint16_t>(sign | h);
	}
	uint32_t h = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
	const uint32_t rest = mant & 0x1FFFu;
	if (rest > 0x1000u || (rest == 0x1000u && (h & 1u) != 0u))
		++h;
	return static_cast<uint16_t>(sign | h);
}

}

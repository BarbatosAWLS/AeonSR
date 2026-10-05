#include "aeon_sr/jitter/shader_offset.hpp"

#include <algorithm>
#include <cstring>

namespace aeon_sr {
namespace {

namespace sm3 {

constexpr uint32_t kEnd = 0x0000FFFFu;
constexpr uint32_t kComment = 0xFFFEu;
constexpr uint32_t kCtab = 0x42415443u;

constexpr uint32_t kOpNop = 0, kOpMov = 1, kOpMad = 4, kOpCall = 25, kOpCallNz = 26, kOpLoop = 27,
	kOpRet = 28, kOpEndLoop = 29, kOpLabel = 30, kOpDcl = 31, kOpRep = 38, kOpEndRep = 39, kOpIf = 40,
	kOpIfc = 41, kOpElse = 42, kOpEndIf = 43, kOpBreak = 44, kOpBreakC = 45, kOpDefB = 47, kOpDefI = 48,
	kOpDef = 81, kOpBreakP = 96;

constexpr uint32_t kRegTemp = 0, kRegConst = 2, kRegRastOut = 4, kRegAttrOut = 5, kRegOutput = 6,
	kRegConst2 = 11, kRegConst3 = 12, kRegConst4 = 13;

constexpr uint32_t kRelative = 1u << 13;

uint32_t reg_type(uint32_t t) noexcept
{
	return ((t >> 28) & 7u) | ((t >> 8) & 0x18u);
}

uint32_t reg_num(uint32_t t) noexcept
{
	return t & 0x7FFu;
}

uint32_t with_reg(uint32_t t, uint32_t type, uint32_t num) noexcept
{
	t &= ~(0x70000000u | 0x00001800u | 0x7FFu);
	return t | ((type & 7u) << 28) | ((type & 0x18u) << 8) | (num & 0x7FFu);
}

uint32_t dst(uint32_t type, uint32_t num, uint32_t mask) noexcept
{
	return with_reg(0x80000000u | (mask << 16), type, num);
}

uint32_t src(uint32_t type, uint32_t num, uint32_t swizzle) noexcept
{
	return with_reg(0x80000000u | (swizzle << 16), type, num);
}

constexpr uint32_t kSwizzleW = 0xFFu, kSwizzleXYXY = 0x44u, kSwizzleXYZW = 0xE4u;

int sm1_params(uint32_t op) noexcept
{
	switch (op) {
	case 0: return 0;
	case 1: return 2;
	case 2: case 3: return 3;
	case 4: return 4;
	case 5: return 3;
	case 6: case 7: return 2;
	case 8: case 9: case 10: case 11: case 12: case 13: return 3;
	case 14: case 15: case 16: return 2;
	case 17: return 3;
	case 19: return 2;
	case 20: case 21: case 22: case 23: case 24: return 3;
	case 31: return 2;
	case 78: case 79: return 2;
	case 81: return 5;
	default: return -1;
	}
}

bool writes_nothing(uint32_t op) noexcept
{
	switch (op) {
	case kOpNop: case kOpCall: case kOpCallNz: case kOpLoop: case kOpRet: case kOpEndLoop: case kOpLabel:
	case kOpRep: case kOpEndRep: case kOpIf: case kOpIfc: case kOpElse: case kOpEndIf: case kOpBreak:
	case kOpBreakC: case kOpBreakP:
		return true;
	default:
		return false;
	}
}

int const_index(uint32_t t) noexcept
{
	const uint32_t n = reg_num(t);
	switch (reg_type(t)) {
	case kRegConst: return static_cast<int>(n);
	case kRegConst2: return static_cast<int>(n + 2048u);
	case kRegConst3: return static_cast<int>(n + 4096u);
	case kRegConst4: return static_cast<int>(n + 6144u);
	default: return -1;
	}
}

struct Scan {
	uint32_t major = 0;
	int pos_out = -1;
	int max_temp = -1;
	bool used[kD3D9OffsetConstants]{};
	bool relative_output = false;
	bool relative_const = false;
	bool ctab = false;
	std::vector<size_t> rast_writes;
	std::vector<std::pair<size_t, uint32_t>> out_writes;
	size_t insert_at = 0;
	size_t end = 0;
};

void mark_const(Scan &s, int index, int count = 1) noexcept
{
	for (int r = index; r >= 0 && r < index + count && r < static_cast<int>(kD3D9OffsetConstants); ++r)
		s.used[r] = true;
}

int matrix_rows(uint32_t op) noexcept
{
	switch (op) {
	case 20: case 22: return 4;
	case 21: case 23: return 3;
	case 24: return 2;
	default: return 1;
	}
}

void read_ctab(const uint32_t *c, size_t len, Scan &s) noexcept
{
	if (len < 1 || c[0] != kCtab)
		return;
	s.ctab = true;
	const auto *base = reinterpret_cast<const uint8_t *>(c + 1);
	const size_t bytes = (len - 1) * 4u;
	if (bytes < 28u)
		return;
	uint32_t constants = 0, info = 0;
	std::memcpy(&constants, base + 12, 4);
	std::memcpy(&info, base + 16, 4);
	if (info > bytes || constants > (bytes - info) / 20u)
		return;
	for (uint32_t k = 0; k < constants; ++k) {
		const uint8_t *ci = base + info + 20u * k;
		uint16_t set = 0, index = 0, count = 0;
		std::memcpy(&set, ci + 4, 2);
		std::memcpy(&index, ci + 6, 2);
		std::memcpy(&count, ci + 8, 2);
		if (set != 2u)
			continue;
		const uint32_t last = std::min<uint32_t>(uint32_t(index) + count, kD3D9OffsetConstants);
		for (uint32_t r = index; r < last; ++r)
			s.used[r] = true;
	}
}

bool scan(const uint32_t *code, size_t n, Scan &s)
{
	if (n < 2 || (code[0] >> 16) != 0xFFFEu)
		return false;
	s.major = (code[0] >> 8) & 0xFFu;
	if (s.major < 1u || s.major > 3u)
		return false;
	size_t p = 1;
	bool in_sub = false;
	int depth = 0;
	size_t main_ret = 0;
	while (p < n) {
		const uint32_t tok = code[p];
		if (tok == kEnd) {
			s.end = p;
			break;
		}
		const uint32_t op = tok & 0xFFFFu;
		if (op == kComment) {
			const size_t len = (tok >> 16) & 0x7FFFu;
			if (p + 1 + len > n)
				return false;
			read_ctab(code + p + 1, len, s);
			p += 1 + len;
			continue;
		}
		if ((tok & 0x80000000u) != 0)
			return false;
		size_t len = (tok >> 24) & 0xFu;
		if (s.major < 2u) {
			const int k = sm1_params(op);
			if (k < 0 || (len != 0 && len != static_cast<size_t>(k)))
				return false;
			len = static_cast<size_t>(k);
		}
		if (p + 1 + len > n)
			return false;
		const uint32_t *prm = code + p + 1;
		switch (op) {
		case kOpLabel:
			in_sub = true;
			break;
		case kOpRet:
			if (!in_sub) {
				if (depth != 0)
					return false;
				if (main_ret == 0)
					main_ret = p;
			}
			break;
		case kOpIf: case kOpIfc: case kOpLoop: case kOpRep:
			if (!in_sub)
				++depth;
			break;
		case kOpEndIf: case kOpEndLoop: case kOpEndRep:
			if (!in_sub)
				--depth;
			break;
		default:
			break;
		}
		if (op == kOpDcl) {
			if (len != 2 || (prm[1] & 0x80000000u) == 0)
				return false;
			if (reg_type(prm[1]) == kRegOutput && (prm[0] & 0x1Fu) == 0u && ((prm[0] >> 16) & 0xFu) == 0u)
				s.pos_out = static_cast<int>(reg_num(prm[1]));
		} else if (op == kOpDef || op == kOpDefI || op == kOpDefB) {
			if (len < 1)
				return false;
			if (op == kOpDef)
				mark_const(s, const_index(prm[0]));
		} else {
			const bool has_dest = !writes_nothing(op);
			size_t i = 0;
			int ordinal = 0;
			while (i < len) {
				const uint32_t t = prm[i];
				if ((t & 0x80000000u) == 0)
					return false;
				const uint32_t type = reg_type(t);
				const bool relative = (t & kRelative) != 0;
				if (type == kRegTemp)
					s.max_temp = std::max(s.max_temp, static_cast<int>(reg_num(t)));
				const int c = const_index(t);
				mark_const(s, c, ordinal == 2 ? matrix_rows(op) : 1);
				if (c >= 0 && relative)
					s.relative_const = true;
				if (has_dest && i == 0) {
					if (s.major < 3u && type == kRegRastOut && reg_num(t) == 0u) {
						if (relative)
							return false;
						s.rast_writes.push_back(p + 1 + i);
					} else if (s.major >= 3u && type == kRegOutput) {
						if (relative)
							s.relative_output = true;
						else
							s.out_writes.emplace_back(p + 1 + i, reg_num(t));
					}
				} else if (type == kRegRastOut || type == kRegAttrOut || type == kRegOutput) {
					return false;
				}
				++i;
				++ordinal;
				if (relative && s.major >= 2u) {
					if (i >= len || (prm[i] & 0x80000000u) == 0)
						return false;
					++i;
				}
			}
		}
		p += 1 + len;
	}
	if (s.end == 0)
		return false;
	s.insert_at = main_ret != 0 ? main_ret : s.end;
	return true;
}

}

namespace sm4 {

constexpr uint32_t kOpLabel = 44, kOpMad = 50, kOpCustomData = 53, kOpMov = 54, kOpRet = 62, kOpRetc = 63,
	kOpDclFirst = 88, kOpDclConstantBuffer = 89, kOpDclIndexRange = 91, kOpDclOutputSiv = 103,
	kOpDclTemps = 104, kOpDclGlobalFlags = 106, kOpDclLast = 106;

constexpr uint32_t kTypeTemp = 0, kTypeOutput = 2, kTypeImm32 = 4, kTypeImm64 = 5, kTypeConstantBuffer = 8;
constexpr uint32_t kNamePosition = 1;

uint32_t op_type(uint32_t t) noexcept
{
	return (t >> 12) & 0xFFu;
}

struct Scan {
	int pos = -1;
	bool used_cb[kD3D10OffsetSlots]{};
	bool relative_output = false;
	size_t temps_at = 0;
	uint32_t temps = 0;
	size_t first_dcl = 0;
	size_t after_cb = 0;
	size_t first_instruction = 0;
	std::vector<size_t> rets;
	std::vector<std::pair<size_t, size_t>> writes;
};

size_t walk_operand(const uint32_t *t, size_t k, size_t end, Scan &s)
{
	if (k >= end)
		return 0;
	const uint32_t tok = t[k];
	size_t n = 1;
	bool ext = (tok >> 31) != 0;
	while (ext) {
		if (k + n >= end)
			return 0;
		ext = (t[k + n] >> 31) != 0;
		++n;
	}
	const uint32_t comps = tok & 3u;
	const uint32_t type = op_type(tok);
	if (type == kTypeImm32)
		n += comps == 1u ? 1u : comps == 2u ? 4u : 0u;
	else if (type == kTypeImm64)
		n += comps == 1u ? 2u : comps == 2u ? 8u : 0u;
	if (k + n > end)
		return 0;
	const uint32_t dim = (tok >> 20) & 3u;
	if (type == kTypeOutput) {
		if (dim == 1u && ((tok >> 22) & 7u) == 0u)
			s.writes.emplace_back(k, k + n);
		else
			s.relative_output = true;
	}
	for (uint32_t d = 0; d < dim; ++d) {
		switch ((tok >> (22u + 3u * d)) & 7u) {
		case 0: n += 1; break;
		case 1: n += 2; break;
		case 2: {
			const size_t sub = walk_operand(t, k + n, end, s);
			if (sub == 0)
				return 0;
			n += sub;
			break;
		}
		case 3: case 4: {
			n += ((tok >> (22u + 3u * d)) & 7u) == 3u ? 1u : 2u;
			const size_t sub = walk_operand(t, k + n, end, s);
			if (sub == 0)
				return 0;
			n += sub;
			break;
		}
		default:
			return 0;
		}
		if (k + n > end)
			return 0;
	}
	return n;
}

bool scan(const uint32_t *t, size_t len, Scan &s)
{
	bool in_sub = false;
	size_t i = 2;
	while (i < len) {
		const uint32_t tok = t[i];
		const uint32_t op = tok & 0x7FFu;
		const size_t ilen = op == kOpCustomData ? (i + 1 < len ? t[i + 1] : 0u) : ((tok >> 24) & 0x7Fu);
		if (ilen == 0 || i + ilen > len)
			return false;
		if (op == kOpCustomData) {
			i += ilen;
			continue;
		}
		if (op >= kOpDclFirst && op <= kOpDclLast) {
			if (s.first_instruction != 0)
				return false;
			if (s.first_dcl == 0 && op != kOpDclGlobalFlags)
				s.first_dcl = i;
			if (op == kOpDclConstantBuffer) {
				if (ilen < 4 || op_type(t[i + 1]) != kTypeConstantBuffer || (t[i + 1] >> 31) != 0)
					return false;
				if (t[i + 2] < kD3D10OffsetSlots)
					s.used_cb[t[i + 2]] = true;
				s.after_cb = i + ilen;
			} else if (op == kOpDclOutputSiv) {
				if (ilen != 4 || op_type(t[i + 1]) != kTypeOutput || (t[i + 1] >> 31) != 0)
					return false;
				if (t[i + 3] == kNamePosition)
					s.pos = static_cast<int>(t[i + 2]);
			} else if (op == kOpDclTemps) {
				if (ilen != 2)
					return false;
				s.temps_at = i;
				s.temps = t[i + 1];
			} else if (op == kOpDclIndexRange) {
				if (ilen < 2 || op_type(t[i + 1]) == kTypeOutput)
					s.relative_output = true;
			}
			i += ilen;
			continue;
		}
		if (s.first_instruction == 0)
			s.first_instruction = i;
		if (op == kOpLabel)
			in_sub = true;
		if ((op == kOpRet || op == kOpRetc) && !in_sub)
			s.rets.push_back(i);
		size_t k = i + 1;
		if ((tok >> 31) != 0) {
			bool more = true;
			while (more) {
				if (k >= i + ilen)
					return false;
				more = (t[k++] >> 31) != 0;
			}
		}
		while (k < i + ilen) {
			const size_t used = walk_operand(t, k, i + ilen, s);
			if (used == 0)
				return false;
			k += used;
		}
		if (k != i + ilen)
			return false;
		i += ilen;
	}
	return i == len;
}

}

uint32_t rotl(uint32_t x, int c) noexcept
{
	return (x << c) | (x >> (32 - c));
}

void md5_block(uint32_t s[4], const uint8_t *block) noexcept
{
	static const uint32_t K[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
	};
	static const int R[64] = {
		7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
		5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
		4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
	};
	uint32_t m[16];
	std::memcpy(m, block, 64);
	uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
	for (int i = 0; i < 64; ++i) {
		uint32_t f;
		int g;
		if (i < 16) {
			f = (b & c) | (~b & d);
			g = i;
		} else if (i < 32) {
			f = (d & b) | (~d & c);
			g = (5 * i + 1) & 15;
		} else if (i < 48) {
			f = b ^ c ^ d;
			g = (3 * i + 5) & 15;
		} else {
			f = c ^ (b | ~d);
			g = (7 * i) & 15;
		}
		const uint32_t next = d;
		d = c;
		c = b;
		b = b + rotl(a + f + K[i] + m[g], R[i]);
		a = next;
	}
	s[0] += a;
	s[1] += b;
	s[2] += c;
	s[3] += d;
}

constexpr uint32_t kDxbc = 0x43425844u;
constexpr uint32_t kShdr = 0x52444853u;
constexpr uint32_t kAon9 = 0x396E6F41u;
constexpr size_t kDxbcHeader = 32u;

uint32_t read32(const uint8_t *p) noexcept
{
	uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}

void write32(uint8_t *p, uint32_t v) noexcept
{
	std::memcpy(p, &v, 4);
}

}

size_t d3d9_shader_dwords(const uint32_t *code, size_t max_dwords) noexcept
{
	if (code == nullptr || max_dwords == 0)
		return 0;
	size_t p = 1;
	while (p < max_dwords) {
		const uint32_t tok = code[p];
		if (tok == sm3::kEnd)
			return p + 1;
		if ((tok & 0xFFFFu) == sm3::kComment)
			p += 1 + ((tok >> 16) & 0x7FFFu);
		else
			++p;
	}
	return 0;
}

bool offset_d3d9_vertex_shader(const uint32_t *code, size_t dwords, uint32_t max_constants,
	std::vector<uint32_t> &out, uint32_t *reg, bool *indexed)
{
	using namespace sm3;
	if (indexed != nullptr)
		*indexed = false;
	if (code == nullptr || reg == nullptr)
		return false;
	Scan s;
	if (!scan(code, dwords, s))
		return false;
	if (s.relative_const && !s.ctab) {
		if (indexed == nullptr)
			return false;
		*indexed = true;
	}
	std::vector<size_t> writes = s.rast_writes;
	uint32_t out_type = kRegRastOut, out_num = 0;
	if (s.major >= 3u) {
		if (s.pos_out < 0 || s.relative_output)
			return false;
		out_type = kRegOutput;
		out_num = static_cast<uint32_t>(s.pos_out);
		for (const auto &w : s.out_writes) {
			if (w.second == out_num)
				writes.push_back(w.first);
		}
	}
	if (writes.empty())
		return false;
	const int temp_limit = s.major >= 3u ? 32 : 12;
	const int temp = s.max_temp + 1;
	if (temp >= temp_limit)
		return false;
	int k = -1;
	for (int r = static_cast<int>(std::min(max_constants, kD3D9OffsetConstants)) - 1; r >= 0 && k < 0; --r) {
		if (!s.used[r])
			k = r;
	}
	if (k < 0)
		return false;

	std::vector<uint32_t> copy(code, code + s.end + 1);
	for (const size_t w : writes)
		copy[w] = with_reg(copy[w], kRegTemp, static_cast<uint32_t>(temp));
	const uint32_t t = static_cast<uint32_t>(temp);
	const bool counted = s.major >= 2u;
	const uint32_t epilog[] = {
		kOpMad | (counted ? 4u << 24 : 0u),
		dst(out_type, out_num, 0x3u),
		src(kRegTemp, t, kSwizzleW),
		src(kRegConst, static_cast<uint32_t>(k), kSwizzleXYXY),
		src(kRegTemp, t, kSwizzleXYXY),
		kOpMov | (counted ? 2u << 24 : 0u),
		dst(out_type, out_num, 0xCu),
		src(kRegTemp, t, kSwizzleXYZW),
	};
	out.clear();
	out.reserve(copy.size() + std::size(epilog));
	out.insert(out.end(), copy.begin(), copy.begin() + static_cast<ptrdiff_t>(s.insert_at));
	out.insert(out.end(), std::begin(epilog), std::end(epilog));
	out.insert(out.end(), copy.begin() + static_cast<ptrdiff_t>(s.insert_at), copy.end());
	*reg = static_cast<uint32_t>(k);
	return true;
}

void dxbc_checksum(const void *code, size_t bytes, uint32_t out[4]) noexcept
{
	out[0] = out[1] = out[2] = out[3] = 0;
	if (code == nullptr || bytes < 20u)
		return;
	const auto *p = static_cast<const uint8_t *>(code) + 20;
	const size_t n = bytes - 20u;
	uint32_t s[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
	const size_t full = n & ~size_t(63);
	for (size_t off = 0; off < full; off += 64)
		md5_block(s, p + off);
	const size_t last = n - full;
	const uint32_t bits = static_cast<uint32_t>(n * 8u);
	const uint32_t tail = (bits >> 2) | 1u;
	uint8_t block[64];
	std::memset(block, 0, sizeof(block));
	if (last >= 56u) {
		std::memcpy(block, p + full, last);
		block[last] = 0x80u;
		md5_block(s, block);
		std::memset(block, 0, sizeof(block));
		write32(block, bits);
		write32(block + 60, tail);
		md5_block(s, block);
	} else {
		write32(block, bits);
		std::memcpy(block + 4, p + full, last);
		block[4 + last] = 0x80u;
		write32(block + 60, tail);
		md5_block(s, block);
	}
	std::memcpy(out, s, sizeof(s));
}

bool offset_dxbc_vertex_shader(const void *code, size_t bytes, std::vector<uint8_t> &out, uint32_t *slot)
{
	using namespace sm4;
	if (code == nullptr || slot == nullptr || bytes < kDxbcHeader)
		return false;
	const auto *in = static_cast<const uint8_t *>(code);
	if (read32(in) != kDxbc)
		return false;
	const uint32_t total = read32(in + 24);
	const uint32_t count = read32(in + 28);
	if (total < kDxbcHeader || total > bytes || count == 0 || count > 64u || kDxbcHeader + 4u * count > total)
		return false;
	int shdr = -1;
	for (uint32_t c = 0; c < count; ++c) {
		const uint32_t off = read32(in + kDxbcHeader + 4u * c);
		if (off > total - 8u || read32(in + off + 4) > total - off - 8u)
			return false;
		if (read32(in + off) == kShdr)
			shdr = static_cast<int>(c);
		if (read32(in + off) == kAon9)
			return false;
	}
	if (shdr < 0)
		return false;
	const uint32_t shdr_off = read32(in + kDxbcHeader + 4u * static_cast<uint32_t>(shdr));
	const uint32_t shdr_size = read32(in + shdr_off + 4);
	if (shdr_size < 8u || (shdr_size & 3u) != 0)
		return false;
	std::vector<uint32_t> t(shdr_size / 4u);
	std::memcpy(t.data(), in + shdr_off + 8, shdr_size);
	if ((t[0] >> 16) != 1u || ((t[0] >> 4) & 0xFu) != 4u)
		return false;
	const size_t len = t[1];
	if (len < 2 || len > t.size())
		return false;

	Scan s;
	if (!scan(t.data(), len, s) || s.pos < 0 || s.relative_output || s.rets.empty() || s.first_instruction == 0)
		return false;
	std::vector<std::pair<size_t, size_t>> pos_writes;
	for (const auto &w : s.writes) {
		if (t[w.second] == static_cast<uint32_t>(s.pos))
			pos_writes.push_back(w);
	}
	if (pos_writes.empty())
		return false;
	int k = -1;
	for (int c = static_cast<int>(kD3D10OffsetSlots) - 1; c >= 0 && k < 0; --c) {
		if (!s.used_cb[c])
			k = c;
	}
	if (k < 0)
		return false;

	const uint32_t temp = s.temps;
	for (const auto &w : pos_writes) {
		t[w.first] = (t[w.first] & ~(0xFFu << 12)) | (kTypeTemp << 12);
		t[w.second] = temp;
	}
	if (s.temps_at != 0)
		t[s.temps_at + 1] = temp + 1u;

	const uint32_t K = static_cast<uint32_t>(k);
	const uint32_t dcl_cb[] = { kOpDclConstantBuffer | (4u << 24), 0x00208E46u, K, 1u };
	const uint32_t dcl_temps[] = { kOpDclTemps | (2u << 24), 1u };
	const uint32_t p = static_cast<uint32_t>(s.pos);
	const uint32_t epilog[] = {
		kOpMad | (10u << 24),
		0x00102032u, p,
		0x00100FF6u, temp,
		0x00208446u, K, 0u,
		0x00100446u, temp,
		kOpMov | (5u << 24),
		0x001020C2u, p,
		0x00100E46u, temp,
	};
	const size_t dcl_at = s.after_cb != 0 ? s.after_cb : s.first_dcl != 0 ? s.first_dcl : s.first_instruction;
	std::vector<uint32_t> nt;
	nt.reserve(len + 8u + s.rets.size() * std::size(epilog));
	size_t ret_next = 0;
	for (size_t i = 0; i < len; ++i) {
		if (i == dcl_at)
			nt.insert(nt.end(), std::begin(dcl_cb), std::end(dcl_cb));
		if (i == s.first_instruction && s.temps_at == 0)
			nt.insert(nt.end(), std::begin(dcl_temps), std::end(dcl_temps));
		if (ret_next < s.rets.size() && i == s.rets[ret_next]) {
			nt.insert(nt.end(), std::begin(epilog), std::end(epilog));
			++ret_next;
		}
		nt.push_back(t[i]);
	}
	nt[1] = static_cast<uint32_t>(nt.size());

	const size_t header = kDxbcHeader + 4u * count;
	size_t new_total = header;
	for (uint32_t c = 0; c < count; ++c) {
		const uint32_t off = read32(in + kDxbcHeader + 4u * c);
		new_total += 8u + (static_cast<int>(c) == shdr ? nt.size() * 4u : read32(in + off + 4));
	}
	out.assign(new_total, 0u);
	std::memcpy(out.data(), in, kDxbcHeader);
	write32(out.data() + 24, static_cast<uint32_t>(new_total));
	size_t at = header;
	for (uint32_t c = 0; c < count; ++c) {
		const uint32_t off = read32(in + kDxbcHeader + 4u * c);
		write32(out.data() + kDxbcHeader + 4u * c, static_cast<uint32_t>(at));
		if (static_cast<int>(c) == shdr) {
			write32(out.data() + at, kShdr);
			write32(out.data() + at + 4, static_cast<uint32_t>(nt.size() * 4u));
			std::memcpy(out.data() + at + 8, nt.data(), nt.size() * 4u);
			at += 8u + nt.size() * 4u;
		} else {
			const uint32_t size = read32(in + off + 4);
			std::memcpy(out.data() + at, in + off, 8u + size);
			at += 8u + size;
		}
	}
	uint32_t hash[4];
	dxbc_checksum(out.data(), out.size(), hash);
	std::memcpy(out.data() + 4, hash, sizeof(hash));
	*slot = K;
	return true;
}

ClipOffset jitter_clip_offset(float tx, float ty, float vw, float vh) noexcept
{
	ClipOffset o;
	if (vw > 0.0f && vh > 0.0f) {
		o.x = 2.0f * tx / vw;
		o.y = -2.0f * ty / vh;
	}
	return o;
}

void offset_projection(const float in[16], ClipOffset o, float out[16]) noexcept
{
	for (int r = 0; r < 4; ++r) {
		const float w = in[r * 4 + 3];
		out[r * 4 + 0] = in[r * 4 + 0] + o.x * w;
		out[r * 4 + 1] = in[r * 4 + 1] + o.y * w;
		out[r * 4 + 2] = in[r * 4 + 2];
		out[r * 4 + 3] = w;
	}
}

}

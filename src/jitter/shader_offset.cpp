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
			if (len > n - p - 1)
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
		if (len > n - p - 1)
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
		if (ilen == 0 || ilen > len - i)
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

namespace vclass {

enum class Reg : uint8_t { None, Temp, Indexed, Position };

enum class Lanes : uint8_t { PerLane, Dot2, Dot3, Dot4, All };

constexpr uint32_t kMaxSources = 8u;
constexpr uint32_t kMaxTemps = 4096u;

struct Dst {
	Reg reg = Reg::None;
	uint32_t index = 0;
	uint8_t mask = 0;
};

struct Src {
	Reg reg = Reg::None;
	uint32_t index = 0;
	uint8_t swizzle[4]{ 0, 1, 2, 3 };
};

enum class Mark : uint8_t { None, Call, Return };

struct Instr {
	Dst dst[2];
	uint32_t dsts = 0;
	Src src[kMaxSources];
	uint32_t srcs = 0;
	Lanes lanes = Lanes::All;
	bool matrix = false;
	bool mul_mad = false;
	bool column = false;
	bool block = false;
	Mark mark = Mark::None;
};

struct Program {
	std::vector<Instr> code;
	size_t subs = SIZE_MAX;
	bool position = false;
	uint32_t temps = 0;
	uint32_t indexed = 0;
};

enum class Kind : uint8_t { Other, Constant, Coordinate };

struct Factor {
	Kind kind = Kind::Other;
	uint8_t swizzle[4]{ 0, 1, 2, 3 };
};

bool replicated(const uint8_t swizzle[4], uint8_t mask) noexcept
{
	int first = -1;
	for (uint32_t c = 0; c < 4u; ++c) {
		if ((mask & (1u << c)) == 0)
			continue;
		if (first < 0)
			first = swizzle[c];
		else if (swizzle[c] != first)
			return false;
	}
	return true;
}

bool column(const Factor &a, const Factor &b, uint8_t mask) noexcept
{
	const auto by = [mask](const Factor &k, const Factor &v) {
		return k.kind == Kind::Constant && v.kind == Kind::Coordinate && !replicated(k.swizzle, mask) &&
			replicated(v.swizzle, mask);
	};
	return by(a, b) || by(b, a);
}

uint8_t lane_bit(uint8_t c) noexcept
{
	return static_cast<uint8_t>(1u << (c & 3u));
}

uint8_t reads(const Instr &in, const Src &s, uint8_t lanes) noexcept
{
	uint8_t m = 0;
	switch (in.lanes) {
	case Lanes::PerLane:
		for (uint8_t c = 0; c < 4u; ++c) {
			if ((lanes & (1u << c)) != 0)
				m |= lane_bit(s.swizzle[c]);
		}
		return m;
	case Lanes::Dot2:
		return static_cast<uint8_t>(lane_bit(s.swizzle[0]) | lane_bit(s.swizzle[1]));
	case Lanes::Dot3:
		return static_cast<uint8_t>(lane_bit(s.swizzle[0]) | lane_bit(s.swizzle[1]) | lane_bit(s.swizzle[2]));
	default:
		return static_cast<uint8_t>(lane_bit(s.swizzle[0]) | lane_bit(s.swizzle[1]) | lane_bit(s.swizzle[2]) |
			lane_bit(s.swizzle[3]));
	}
}

bool add_src(Instr &in, const Src &s) noexcept
{
	if (in.srcs == kMaxSources)
		return false;
	in.src[in.srcs++] = s;
	return true;
}

void note_reg(Program &p, Reg reg, uint32_t index) noexcept
{
	if (reg == Reg::Temp)
		p.temps = std::max(p.temps, index + 1u);
	else if (reg == Reg::Indexed)
		p.indexed = std::max(p.indexed, index + 1u);
}

JitterVertexClass weigh(const Program &p)
{
	if (!p.position)
		return JitterVertexClass::Unknown;
	std::vector<uint8_t> temps(p.temps, 0u), indexed(p.indexed, 0u);
	uint8_t position = 0xFu;
	std::vector<uint8_t> taken(p.code.size(), 0u);
	const auto live = [&](Reg reg, uint32_t index) -> uint8_t * {
		switch (reg) {
		case Reg::Temp: return index < temps.size() ? &temps[index] : nullptr;
		case Reg::Indexed: return index < indexed.size() ? &indexed[index] : nullptr;
		case Reg::Position: return &position;
		default: return nullptr;
		}
	};
	const auto step = [&](size_t k, bool replaces) -> bool {
		const Instr &in = p.code[k];
		if (in.mark == Mark::Return) {
			const bool more = position != 0xFu;
			position = 0xFu;
			return more;
		}
		uint8_t lanes = 0;
		for (uint32_t d = 0; d < in.dsts; ++d) {
			uint8_t *const l = live(in.dst[d].reg, in.dst[d].index);
			if (l == nullptr)
				continue;
			lanes |= *l & in.dst[d].mask;
			if (replaces && in.dst[d].reg != Reg::Indexed)
				*l &= static_cast<uint8_t>(~in.dst[d].mask);
		}
		if (lanes == 0)
			return false;
		taken[k] = 1u;
		bool more = false;
		for (uint32_t s = 0; s < in.srcs; ++s) {
			uint8_t *const l = live(in.src[s].reg, in.src[s].index);
			if (l == nullptr || l == &position)
				continue;
			const uint8_t now = static_cast<uint8_t>(*l | reads(in, in.src[s], lanes));
			more = more || now != *l;
			*l = now;
		}
		return more;
	};
	const auto once = [&](size_t k) { return !p.code[k].block && p.code[k].mark == Mark::None; };
	const size_t subs = std::min(p.subs, p.code.size());
	for (size_t k = subs; k-- > 0;) {
		if (once(k)) {
			step(k, true);
			continue;
		}
		size_t first = k;
		while (first > 0 && !once(first - 1))
			--first;
		bool calls = false;
		for (size_t j = first; j <= k; ++j)
			calls = calls || p.code[j].mark == Mark::Call;
		for (bool more = true; more;) {
			more = false;
			for (size_t j = k + 1; j-- > first;)
				more = step(j, false) || more;
			for (size_t j = p.code.size(); calls && j-- > subs;)
				more = step(j, false) || more;
		}
		k = first;
	}
	uint32_t matrices = 0, mul_mads = 0, columns = 0;
	for (size_t k = 0; k < p.code.size(); ++k) {
		if (taken[k] == 0u)
			continue;
		matrices += p.code[k].matrix ? 1u : 0u;
		mul_mads += p.code[k].mul_mad ? 1u : 0u;
		columns += p.code[k].column ? 1u : 0u;
	}
	return matrices != 0u || mul_mads >= 3u || columns >= 2u ? JitterVertexClass::Transformed
		: JitterVertexClass::Screen;
}

bool read_d3d9(const uint32_t *code, size_t n, Program &out)
{
	using namespace sm3;
	if (code == nullptr || n < 2 || (code[0] >> 16) != 0xFFFEu)
		return false;
	const uint32_t major = (code[0] >> 8) & 0xFFu;
	if (major < 1u || major > 3u)
		return false;
	constexpr uint32_t kRegInput = 1, kRegPredicate = 19, kOpMul = 5, kOpDp3 = 8, kOpDp4 = 9, kOpM4x4 = 20,
		kOpM3x3 = 23;
	std::vector<uint8_t> literal(8192u, 0u);
	int pos_out = -1;
	for (int pass = 0; pass < 2; ++pass) {
		size_t p = 1;
		bool ended = false;
		uint32_t depth = 0;
		bool sub = false;
		while (p < n) {
			const uint32_t tok = code[p];
			if (tok == kEnd) {
				ended = true;
				break;
			}
			const uint32_t op = tok & 0xFFFFu;
			if (op == kComment) {
				p += 1 + ((tok >> 16) & 0x7FFFu);
				continue;
			}
			if ((tok & 0x80000000u) != 0)
				return false;
			size_t len = (tok >> 24) & 0xFu;
			if (major < 2u) {
				const int k = sm1_params(op);
				if (k < 0 || (len != 0 && len != static_cast<size_t>(k)))
					return false;
				len = static_cast<size_t>(k);
			}
			if (len > n - p - 1)
				return false;
			const uint32_t *const prm = code + p + 1;
			p += 1 + len;
			if (op == kOpDcl) {
				if (len != 2 || (prm[1] & 0x80000000u) == 0)
					return false;
				if (pass == 0 && reg_type(prm[1]) == kRegOutput && (prm[0] & 0x1Fu) == 0u &&
					((prm[0] >> 16) & 0xFu) == 0u)
					pos_out = static_cast<int>(reg_num(prm[1]));
				continue;
			}
			if (op == kOpDef || op == kOpDefI || op == kOpDefB) {
				if (len < 1)
					return false;
				const int c = const_index(prm[0]);
				if (pass == 0 && op == kOpDef && c >= 0 && static_cast<size_t>(c) < literal.size())
					literal[static_cast<size_t>(c)] = 1u;
				continue;
			}
			if (pass == 0)
				continue;
			switch (op) {
			case kOpIf: case kOpIfc: case kOpLoop: case kOpRep:
				++depth;
				continue;
			case kOpEndIf: case kOpEndLoop: case kOpEndRep:
				if (depth == 0)
					return false;
				--depth;
				continue;
			case kOpLabel:
				if (!sub)
					out.subs = out.code.size();
				sub = true;
				depth = 0;
				continue;
			case kOpCall: case kOpCallNz: case kOpRet:
				if (!sub) {
					Instr mark;
					mark.mark = op == kOpRet ? Mark::Return : Mark::Call;
					out.code.push_back(mark);
				}
				continue;
			default:
				break;
			}
			if (op == kOpNop || writes_nothing(op))
				continue;
			Instr in;
			in.block = sub || depth != 0 || (major >= 2u && (tok & (1u << 28)) != 0);
			switch (op) {
			case 1: case 2: case 3: case 4: case 5: case 10: case 11: case 12: case 13: case 18: case 19: case 35:
			case 80: case 88:
				in.lanes = Lanes::PerLane;
				break;
			case kOpDp3: case 22: case kOpM3x3:
				in.lanes = Lanes::Dot3;
				break;
			case kOpDp4: case kOpM4x4: case 21:
				in.lanes = Lanes::Dot4;
				break;
			default:
				in.lanes = Lanes::All;
				break;
			}
			bool constant = false;
			Factor factors[2];
			uint32_t nfactors = 0;
			size_t i = 0;
			while (i < len) {
				const uint32_t t = prm[i];
				if ((t & 0x80000000u) == 0)
					return false;
				const uint32_t type = reg_type(t);
				const bool relative = (t & kRelative) != 0;
				Factor f;
				for (uint32_t c = 0; c < 4u; ++c)
					f.swizzle[c] = static_cast<uint8_t>((t >> (16u + 2u * c)) & 3u);
				if (i == 0) {
					Dst d;
					d.mask = static_cast<uint8_t>((t >> 16) & 0xFu);
					if (major < 3u && type == kRegRastOut && reg_num(t) == 0u) {
						if (relative)
							return false;
						d.reg = Reg::Position;
					} else if (major >= 3u && type == kRegOutput) {
						if (relative)
							return false;
						if (static_cast<int>(reg_num(t)) == pos_out)
							d.reg = Reg::Position;
					} else if (type == kRegTemp) {
						d.reg = Reg::Temp;
						d.index = reg_num(t);
					}
					if (d.reg == Reg::Position)
						out.position = true;
					note_reg(out, d.reg, d.index);
					in.dst[in.dsts++] = d;
				} else if (type == kRegTemp) {
					Src s;
					s.reg = Reg::Temp;
					s.index = reg_num(t);
					std::memcpy(s.swizzle, f.swizzle, 4);
					note_reg(out, s.reg, s.index);
					if (!add_src(in, s))
						return false;
					f.kind = Kind::Coordinate;
				} else if (type == kRegInput) {
					f.kind = Kind::Coordinate;
				} else if (type != kRegPredicate) {
					const int c = const_index(t);
					if (c >= 0 && (relative || static_cast<size_t>(c) >= literal.size() || literal[static_cast<size_t>(c)] == 0u)) {
						constant = true;
						f.kind = Kind::Constant;
					}
				}
				if (i != 0 && type != kRegPredicate && nfactors < 2u)
					factors[nfactors++] = f;
				++i;
				if (relative && major >= 2u) {
					if (i >= len || (prm[i] & 0x80000000u) == 0)
						return false;
					++i;
				}
			}
			if (in.dsts == 0)
				continue;
			in.matrix = constant && (op == kOpDp3 || op == kOpDp4 || (op >= kOpM4x4 && op <= kOpM3x3));
			in.mul_mad = constant && (op == kOpMad || op == kOpMul);
			in.column = in.mul_mad && nfactors == 2u && column(factors[0], factors[1], in.dst[0].mask);
			out.code.push_back(in);
		}
		if (!ended)
			return false;
	}
	return out.temps <= kMaxTemps;
}

namespace sm4ops {

constexpr uint32_t kCall = 4, kCallC = 5, kDp2 = 15, kDp3 = 16, kDp4 = 17, kEndIf = 21, kEndLoop = 22,
	kEndSwitch = 23, kIf = 31, kLabel = 44, kLoop = 48, kMad = 50, kMul = 56, kRet = 62, kRetC = 63,
	kSwitch = 76, kCustomData = 53, kInterfaceCall = 120, kDclOutputSiv = 103;
constexpr uint32_t kTypeTemp = 0, kTypeInput = 1, kTypeOutput = 2, kTypeIndexableTemp = 3, kTypeImm32 = 4,
	kTypeImm64 = 5, kTypeConstantBuffer = 8;

bool declaration(uint32_t op) noexcept
{
	return (op >= 88u && op <= 106u) || (op >= 113u && op <= 116u) || (op >= 143u && op <= 162u) || op == 206u;
}

uint32_t destinations(uint32_t op) noexcept
{
	switch (op) {
	case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 13: case 18: case 19: case 20:
	case 21: case 22: case 23: case 31: case 44: case 48: case 58: case 62: case 63: case 76: case 117: case 118:
	case 119: case 190: case 207: case 208:
		return 0;
	case 38: case 77: case 78: case 81: case 132: case 133: case 142:
		return 2;
	default:
		return 1;
	}
}

Lanes lanes(uint32_t op) noexcept
{
	switch (op) {
	case 0: case 1: case 14: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 32: case 33:
	case 34: case 35: case 36: case 37: case 39: case 40: case 41: case 42: case 43: case 47: case 49: case 50:
	case 51: case 52: case 54: case 55: case 56: case 57: case 59: case 60: case 64: case 65: case 66: case 67:
	case 68: case 75: case 79: case 80: case 82: case 83: case 84: case 85: case 86: case 87: case 129:
		return Lanes::PerLane;
	case kDp2: return Lanes::Dot2;
	case kDp3: return Lanes::Dot3;
	case kDp4: return Lanes::Dot4;
	default: return Lanes::All;
	}
}

struct Operand {
	uint32_t type = 0;
	uint32_t comps = 0;
	uint8_t mask = 0;
	uint8_t swizzle[4]{ 0, 1, 2, 3 };
	uint32_t dim = 0;
	uint32_t index0 = 0;
	bool relative0 = false;
};

size_t operand(const uint32_t *t, size_t k, size_t end, Operand &o, Instr &in, Program &p, int depth = 0)
{
	if (k >= end || depth > 4)
		return 0;
	const uint32_t tok = t[k];
	size_t n = 1;
	for (bool ext = (tok >> 31) != 0; ext;) {
		if (k + n >= end)
			return 0;
		ext = (t[k + n] >> 31) != 0;
		++n;
	}
	o.comps = tok & 3u;
	if (o.comps == 3u)
		return 0;
	if (o.comps == 2u) {
		switch ((tok >> 2) & 3u) {
		case 0:
			o.mask = static_cast<uint8_t>((tok >> 4) & 0xFu);
			break;
		case 1:
			o.mask = 0xFu;
			for (uint32_t c = 0; c < 4u; ++c)
				o.swizzle[c] = static_cast<uint8_t>((tok >> (4u + 2u * c)) & 3u);
			break;
		case 2:
			o.mask = 0xFu;
			for (uint8_t &s : o.swizzle)
				s = static_cast<uint8_t>((tok >> 4) & 3u);
			break;
		default:
			return 0;
		}
	} else if (o.comps == 1u) {
		o.mask = 1u;
		for (uint8_t &s : o.swizzle)
			s = 0u;
	}
	o.type = (tok >> 12) & 0xFFu;
	if (o.type == kTypeImm32)
		n += o.comps == 1u ? 1u : o.comps == 2u ? 4u : 0u;
	else if (o.type == kTypeImm64)
		n += o.comps == 1u ? 2u : o.comps == 2u ? 8u : 0u;
	if (k + n > end)
		return 0;
	o.dim = (tok >> 20) & 3u;
	for (uint32_t d = 0; d < o.dim; ++d) {
		const uint32_t rep = (tok >> (22u + 3u * d)) & 7u;
		if (rep > 4u)
			return 0;
		if (rep == 0u || rep == 3u) {
			if (k + n >= end)
				return 0;
			if (d == 0u)
				o.index0 = t[k + n];
			n += 1;
		} else if (rep == 1u || rep == 4u) {
			n += 2;
		}
		if (rep >= 2u) {
			Operand sub;
			const size_t used = operand(t, k + n, end, sub, in, p, depth + 1);
			if (used == 0)
				return 0;
			if (sub.type == kTypeTemp || sub.type == kTypeIndexableTemp) {
				Src s;
				s.reg = sub.type == kTypeTemp ? Reg::Temp : Reg::Indexed;
				s.index = sub.index0;
				std::memcpy(s.swizzle, sub.swizzle, 4);
				note_reg(p, s.reg, s.index);
				if (!add_src(in, s))
					return 0;
			}
			n += used;
			if (d == 0u)
				o.relative0 = true;
		}
		if (k + n > end)
			return 0;
	}
	return n;
}

}

bool read_sm4(const uint32_t *t, size_t len, Program &out)
{
	using namespace sm4ops;
	int pos = -1;
	uint32_t depth = 0;
	bool sub = false;
	size_t i = 2;
	while (i < len) {
		const uint32_t tok = t[i];
		const uint32_t op = tok & 0x7FFu;
		const size_t ilen = op == kCustomData ? (i + 1 < len ? t[i + 1] : 0u) : ((tok >> 24) & 0x7Fu);
		if (ilen == 0 || ilen > len - i)
			return false;
		const size_t end = i + ilen;
		if (op == kCustomData) {
			i = end;
			continue;
		}
		if (declaration(op)) {
			if (op == kDclOutputSiv && ilen >= 4 && sm4::op_type(t[i + 1]) == kTypeOutput && (t[i + 1] >> 31) == 0 &&
				t[i + 3] == sm4::kNamePosition)
				pos = static_cast<int>(t[i + 2]);
			i = end;
			continue;
		}
		if (op == kInterfaceCall)
			return false;
		switch (op) {
		case kIf: case kLoop: case kSwitch:
			++depth;
			break;
		case kEndIf: case kEndLoop: case kEndSwitch:
			if (depth == 0)
				return false;
			--depth;
			break;
		case kLabel:
			if (!sub)
				out.subs = out.code.size();
			sub = true;
			depth = 0;
			break;
		default:
			break;
		}
		size_t k = i + 1;
		for (bool ext = (tok >> 31) != 0; ext;) {
			if (k >= end)
				return false;
			ext = (t[k] >> 31) != 0;
			++k;
		}
		Instr in;
		in.lanes = lanes(op);
		in.block = sub || depth != 0;
		const uint32_t writes = destinations(op);
		bool constant = false;
		Factor factors[2];
		uint32_t nfactors = 0;
		for (uint32_t index = 0; k < end; ++index) {
			Operand o;
			const size_t used = operand(t, k, end, o, in, out);
			if (used == 0)
				return false;
			k += used;
			if (index < writes) {
				if (in.dsts == 2u)
					return false;
				Dst d;
				d.mask = static_cast<uint8_t>(o.comps == 2u ? o.mask : o.comps == 1u ? 1u : 0u);
				if (o.type == kTypeTemp) {
					d.reg = Reg::Temp;
					d.index = o.index0;
				} else if (o.type == kTypeIndexableTemp) {
					d.reg = Reg::Indexed;
					d.index = o.index0;
				} else if (o.type == kTypeOutput) {
					if (o.relative0 || o.dim != 1u)
						return false;
					if (static_cast<int>(o.index0) == pos)
						d.reg = Reg::Position;
				}
				if (d.reg == Reg::Position)
					out.position = true;
				note_reg(out, d.reg, d.index);
				in.dst[in.dsts++] = d;
				continue;
			}
			Factor f;
			std::memcpy(f.swizzle, o.swizzle, 4);
			if (o.type == kTypeConstantBuffer) {
				constant = true;
				f.kind = Kind::Constant;
			} else if (o.type == kTypeTemp || o.type == kTypeIndexableTemp) {
				Src s;
				s.reg = o.type == kTypeTemp ? Reg::Temp : Reg::Indexed;
				s.index = o.index0;
				std::memcpy(s.swizzle, o.swizzle, 4);
				note_reg(out, s.reg, s.index);
				if (!add_src(in, s))
					return false;
				f.kind = Kind::Coordinate;
			} else if (o.type == kTypeInput) {
				f.kind = Kind::Coordinate;
			}
			if (nfactors < 2u)
				factors[nfactors++] = f;
		}
		if (k != end)
			return false;
		if (!sub && (op == kCall || op == kCallC || op == kRet || op == kRetC)) {
			Instr mark;
			mark.mark = op == kRet || op == kRetC ? Mark::Return : Mark::Call;
			out.code.push_back(mark);
		}
		in.matrix = constant && (op == kDp2 || op == kDp3 || op == kDp4);
		in.mul_mad = constant && (op == kMad || op == kMul);
		in.column = in.mul_mad && nfactors == 2u && column(factors[0], factors[1], in.dst[0].mask);
		if (in.dsts != 0)
			out.code.push_back(in);
		i = end;
	}
	return i == len && out.temps <= kMaxTemps && out.indexed <= kMaxTemps;
}

}

namespace wterms {

using namespace sm3;

constexpr uint32_t kRegInput = 1, kRegPredicate = 19;
constexpr uint32_t kOpAdd = 2, kOpSub = 3, kOpMul = 5, kOpDp3 = 8, kOpDp4 = 9, kOpM4x4 = 20, kOpM4x3 = 21,
	kOpM3x4 = 22, kOpM3x3 = 23, kOpM3x2 = 24;
constexpr uint32_t kSaturate = 1u << 20;
constexpr uint32_t kModNone = 0u, kModNegate = 1u;
constexpr int kDeviceConstants = static_cast<int>(kD3D9OffsetConstants);
constexpr int kBudget = 256;

struct Operand {
	uint32_t type = 0;
	uint32_t num = 0;
	uint8_t swizzle[4]{ 0, 1, 2, 3 };
	bool relative = false;
	bool literal = false;
	bool modified = false;
};

struct Instr {
	uint32_t op = 0;
	bool once = true;
	bool saturate = false;
	bool relative = false;
	uint32_t type = 0, num = 0;
	uint8_t mask = 0;
	Operand src[3];
	uint32_t srcs = 0;
	bool more = false;
};

struct Shader {
	std::vector<Instr> code;
	uint32_t pos_type = 0, pos_num = 0;
	bool position = false;
};

bool constant(const Operand &o) noexcept
{
	return o.type == kRegConst || o.type == kRegConst2 || o.type == kRegConst3 || o.type == kRegConst4;
}

bool read(const uint32_t *code, size_t n, Shader &out)
{
	if (code == nullptr || n < 2 || (code[0] >> 16) != 0xFFFEu)
		return false;
	const uint32_t major = (code[0] >> 8) & 0xFFu;
	if (major < 1u || major > 3u)
		return false;
	out.pos_type = major < 3u ? kRegRastOut : kRegOutput;
	out.position = major < 3u;
	std::vector<uint8_t> literal(8192u, 0u);
	for (int pass = 0; pass < 2; ++pass) {
		size_t p = 1;
		uint32_t depth = 0;
		bool ended = false, main_ended = false;
		while (p < n) {
			const uint32_t tok = code[p];
			if (tok == kEnd) {
				ended = true;
				break;
			}
			const uint32_t op = tok & 0xFFFFu;
			if (op == kComment) {
				p += 1 + ((tok >> 16) & 0x7FFFu);
				continue;
			}
			if ((tok & 0x80000000u) != 0)
				return false;
			size_t len = (tok >> 24) & 0xFu;
			if (major < 2u) {
				const int k = sm1_params(op);
				if (k < 0 || (len != 0 && len != static_cast<size_t>(k)))
					return false;
				len = static_cast<size_t>(k);
			}
			if (len > n - p - 1)
				return false;
			const uint32_t *const prm = code + p + 1;
			p += 1 + len;
			if (op == kOpDcl) {
				if (len != 2 || (prm[1] & 0x80000000u) == 0)
					return false;
				if (pass == 0 && major >= 3u && reg_type(prm[1]) == kRegOutput && (prm[0] & 0x1Fu) == 0u &&
					((prm[0] >> 16) & 0xFu) == 0u) {
					out.pos_num = reg_num(prm[1]);
					out.position = true;
				}
				continue;
			}
			if (op == kOpDef || op == kOpDefI || op == kOpDefB) {
				if (len < 1)
					return false;
				const int c = const_index(prm[0]);
				if (pass == 0 && op == kOpDef && c >= 0 && static_cast<size_t>(c) < literal.size())
					literal[static_cast<size_t>(c)] = 1u;
				continue;
			}
			if (pass == 0 || main_ended)
				continue;
			switch (op) {
			case kOpIf: case kOpIfc: case kOpLoop: case kOpRep:
				++depth;
				continue;
			case kOpEndIf: case kOpEndLoop: case kOpEndRep:
				if (depth == 0)
					return false;
				--depth;
				continue;
			case kOpRet: case kOpLabel:
				if (depth != 0)
					return false;
				main_ended = true;
				continue;
			case kOpCall: case kOpCallNz:
				return false;
			default:
				break;
			}
			if (op == kOpNop || writes_nothing(op))
				continue;
			Instr in;
			in.op = op;
			in.once = depth == 0 && !(major >= 2u && (tok & (1u << 28)) != 0);
			size_t i = 0;
			bool first = true;
			while (i < len) {
				const uint32_t t = prm[i];
				if ((t & 0x80000000u) == 0)
					return false;
				const uint32_t type = reg_type(t);
				const bool relative = (t & kRelative) != 0;
				if (first) {
					in.type = type;
					in.num = reg_num(t);
					in.mask = static_cast<uint8_t>((t >> 16) & 0xFu);
					in.saturate = (t & kSaturate) != 0;
					in.relative = relative;
					if (relative && type == kRegOutput)
						return false;
					first = false;
				} else if (type != kRegPredicate) {
					Operand o;
					o.type = type;
					o.num = reg_num(t);
					for (uint32_t c = 0; c < 4u; ++c)
						o.swizzle[c] = static_cast<uint8_t>((t >> (16u + 2u * c)) & 3u);
					o.relative = relative;
					const uint32_t mod = (t >> 24) & 0xFu;
					o.modified = mod != kModNone && mod != kModNegate;
					const int c = const_index(t);
					o.literal = c >= 0 && !relative && static_cast<size_t>(c) < literal.size() &&
						literal[static_cast<size_t>(c)] != 0u;
					if (in.srcs < 3u)
						in.src[in.srcs++] = o;
					else
						in.more = true;
				}
				++i;
				if (relative && major >= 2u) {
					if (i >= len || (prm[i] & 0x80000000u) == 0)
						return false;
					++i;
				}
			}
			if (!first)
				out.code.push_back(in);
		}
		if (!ended)
			return false;
	}
	return out.position;
}

struct Tracer {
	const Shader &s;
	D3D9WTerms &out;
	int budget = kBudget;

	bool name(int reg, uint8_t lane) noexcept
	{
		if (reg < 0 || reg >= kDeviceConstants)
			return false;
		for (uint32_t i = 0; i < out.count; ++i) {
			if (out.reg[i] == reg && out.lane[i] == lane)
				return true;
		}
		if (out.count == D3D9WTerms::kMax)
			return false;
		out.reg[out.count] = static_cast<uint16_t>(reg);
		out.lane[out.count] = lane;
		++out.count;
		return true;
	}

	bool product(const Operand &a, uint8_t la, const Operand &b, uint8_t lb) noexcept
	{
		if (a.modified || b.modified)
			return false;
		const bool ka = constant(a), kb = constant(b);
		if (ka && kb)
			return !a.relative && !b.relative;
		if (ka == kb)
			return false;
		const Operand &k = ka ? a : b;
		const Operand &v = ka ? b : a;
		const uint8_t lk = ka ? la : lb, lv = ka ? lb : la;
		if (v.type != kRegTemp && v.type != kRegInput)
			return false;
		if (k.relative)
			return false;
		if (lv == 3u)
			return true;
		if (k.literal)
			return false;
		return name(k.type == kRegConst ? static_cast<int>(k.num) : -1, lk);
	}

	bool addend(const Operand &o, uint8_t lane, size_t before) noexcept
	{
		if (o.modified)
			return false;
		if (constant(o))
			return !o.relative;
		if (o.type == kRegInput)
			return lane == 3u;
		if (o.type == kRegTemp && !o.relative)
			return value(kRegTemp, o.num, lane, before);
		return false;
	}

	bool value(uint32_t type, uint32_t num, uint8_t lane, size_t before) noexcept
	{
		if (--budget < 0)
			return false;
		for (size_t k = before; k-- > 0;) {
			const Instr &in = s.code[k];
			if (in.type != type || in.num != num || (in.mask & (1u << lane)) == 0)
				continue;
			if (!in.once || in.relative || in.saturate || in.more)
				return false;
			return written(k, lane);
		}
		return false;
	}

	bool written(size_t k, uint8_t lane) noexcept
	{
		const Instr &in = s.code[k];
		const auto at = [&in](uint32_t i, uint8_t l) { return in.src[i].swizzle[l]; };
		switch (in.op) {
		case kOpMov:
			return in.srcs == 1u && addend(in.src[0], at(0, lane), k);
		case kOpAdd: case kOpSub:
			return in.srcs == 2u && addend(in.src[0], at(0, lane), k) && addend(in.src[1], at(1, lane), k);
		case kOpMul:
			return in.srcs == 2u && product(in.src[0], at(0, lane), in.src[1], at(1, lane));
		case kOpMad:
			return in.srcs == 3u && product(in.src[0], at(0, lane), in.src[1], at(1, lane)) &&
				addend(in.src[2], at(2, lane), k);
		case kOpDp3: case kOpDp4: {
			if (in.srcs != 2u)
				return false;
			const uint8_t dots = static_cast<uint8_t>(in.op == kOpDp3 ? 3u : 4u);
			for (uint8_t i = 0; i < dots; ++i) {
				if (!product(in.src[0], at(0, i), in.src[1], at(1, i)))
					return false;
			}
			return true;
		}
		case kOpM4x4: case kOpM4x3: case kOpM3x4: case kOpM3x3: case kOpM3x2: {
			const uint32_t rows = in.op == kOpM3x2 ? 2u : (in.op == kOpM4x3 || in.op == kOpM3x3) ? 3u : 4u;
			const uint8_t dots = static_cast<uint8_t>((in.op == kOpM4x4 || in.op == kOpM4x3) ? 4u : 3u);
			if (in.srcs != 2u || lane >= rows || !constant(in.src[1]))
				return false;
			Operand row = in.src[1];
			row.num += lane;
			for (uint8_t i = 0; i < dots; ++i) {
				if (!product(in.src[0], at(0, i), row, i))
					return false;
			}
			return true;
		}
		default:
			return false;
		}
	}
};

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

bool dxbc_program(const void *code, size_t bytes, std::vector<uint32_t> &t, size_t &len)
{
	if (code == nullptr || bytes < kDxbcHeader)
		return false;
	const auto *in = static_cast<const uint8_t *>(code);
	if (read32(in) != kDxbc)
		return false;
	const uint32_t total = read32(in + 24);
	const uint32_t count = read32(in + 28);
	if (total < kDxbcHeader || total > bytes || count == 0 || count > 64u || kDxbcHeader + 4u * count > total)
		return false;
	constexpr uint32_t kShex = 0x58454853u;
	uint32_t program = 0;
	for (uint32_t c = 0; c < count; ++c) {
		const uint32_t off = read32(in + kDxbcHeader + 4u * c);
		if (off > total - 8u || read32(in + off + 4) > total - off - 8u)
			return false;
		if (read32(in + off) == kShdr || read32(in + off) == kShex)
			program = off;
	}
	if (program == 0)
		return false;
	const uint32_t size = read32(in + program + 4);
	if (size < 8u || (size & 3u) != 0)
		return false;
	t.assign(size / 4u, 0u);
	std::memcpy(t.data(), in + program + 8, size);
	const uint32_t major = (t[0] >> 4) & 0xFu;
	if ((t[0] >> 16) != 1u || major < 4u || major > 5u)
		return false;
	len = t[1];
	return len >= 2 && len <= t.size();
}

namespace wterms4 {

constexpr uint32_t kOpAdd = 0, kOpCall = 4, kOpCallC = 5, kOpDp2 = 15, kOpDp3 = 16, kOpDp4 = 17, kOpEndIf = 21,
	kOpEndLoop = 22, kOpEndSwitch = 23, kOpIf = 31, kOpLabel = 44, kOpLoop = 48, kOpMad = 50, kOpCustomData = 53,
	kOpMov = 54, kOpMul = 56, kOpRet = 62, kOpRetC = 63, kOpSwitch = 76, kOpInterfaceCall = 120,
	kOpDclOutputSiv = 103, kOpDclConstantBuffer = 89;
constexpr uint32_t kTypeTemp = 0, kTypeInput = 1, kTypeOutput = 2, kTypeImm32 = 4, kTypeImm64 = 5,
	kTypeConstantBuffer = 8, kTypeImmediateConstantBuffer = 9;
constexpr uint32_t kNamePosition = 1;
constexpr uint32_t kSaturate = 1u << 13;
constexpr int kBudget = 256;

struct Operand {
	uint32_t type = 0;
	uint32_t index0 = 0;
	uint32_t index1 = 0;
	uint32_t index2 = 0;
	uint32_t dim = 0;
	uint8_t mask = 0;
	uint8_t swizzle[4]{ 0, 1, 2, 3 };
	bool relative = false;
	bool modified = false;
};

struct Instr {
	uint32_t op = 0;
	bool once = true;
	bool saturate = false;
	Operand dst[2];
	uint32_t dsts = 0;
	Operand src[3];
	uint32_t srcs = 0;
	bool more = false;
};

struct Shader {
	std::vector<Instr> code;
	int pos = -1;
	std::vector<std::pair<uint32_t, uint32_t>> spaces;
};

size_t operand(const uint32_t *t, size_t k, size_t end, Operand &o, int depth = 0)
{
	if (k >= end || depth > 4)
		return 0;
	const uint32_t tok = t[k];
	size_t n = 1;
	for (bool ext = (tok >> 31) != 0; ext;) {
		if (k + n >= end)
			return 0;
		const uint32_t e = t[k + n];
		ext = (e >> 31) != 0;
		if ((e & 0x3Fu) == 1u) {
			if (((e >> 6) & 0xFFu) > 3u)
				o.modified = true;
		} else if ((e & 0x3Fu) != 0u) {
			o.modified = true;
		}
		++n;
	}
	const uint32_t comps = tok & 3u;
	if (comps == 3u)
		return 0;
	if (comps == 2u) {
		switch ((tok >> 2) & 3u) {
		case 0:
			o.mask = static_cast<uint8_t>((tok >> 4) & 0xFu);
			break;
		case 1:
			o.mask = 0xFu;
			for (uint32_t c = 0; c < 4u; ++c)
				o.swizzle[c] = static_cast<uint8_t>((tok >> (4u + 2u * c)) & 3u);
			break;
		case 2:
			o.mask = 0xFu;
			for (uint8_t &s : o.swizzle)
				s = static_cast<uint8_t>((tok >> 4) & 3u);
			break;
		default:
			return 0;
		}
	} else if (comps == 1u) {
		o.mask = 1u;
		for (uint8_t &s : o.swizzle)
			s = 0u;
	}
	o.type = (tok >> 12) & 0xFFu;
	if (o.type == kTypeImm32)
		n += comps == 1u ? 1u : comps == 2u ? 4u : 0u;
	else if (o.type == kTypeImm64)
		n += comps == 1u ? 2u : comps == 2u ? 8u : 0u;
	if (k + n > end)
		return 0;
	o.dim = (tok >> 20) & 3u;
	for (uint32_t d = 0; d < o.dim; ++d) {
		const uint32_t rep = (tok >> (22u + 3u * d)) & 7u;
		if (rep > 4u)
			return 0;
		if (rep == 0u || rep == 3u) {
			if (k + n >= end)
				return 0;
			if (d == 0u)
				o.index0 = t[k + n];
			else if (d == 1u)
				o.index1 = t[k + n];
			else
				o.index2 = t[k + n];
			n += 1;
		} else if (rep == 1u || rep == 4u) {
			n += 2;
			o.relative = true;
		}
		if (rep >= 2u) {
			Operand sub;
			const size_t used = operand(t, k + n, end, sub, depth + 1);
			if (used == 0)
				return 0;
			n += used;
			o.relative = true;
		}
		if (k + n > end)
			return 0;
	}
	return n;
}

bool read(const uint32_t *t, size_t len, Shader &out)
{
	using vclass::sm4ops::declaration;
	using vclass::sm4ops::destinations;
	uint32_t depth = 0;
	bool ended = false;
	size_t i = 2;
	while (i < len) {
		const uint32_t tok = t[i];
		const uint32_t op = tok & 0x7FFu;
		const size_t ilen = op == kOpCustomData ? (i + 1 < len ? t[i + 1] : 0u) : ((tok >> 24) & 0x7Fu);
		if (ilen == 0 || ilen > len - i)
			return false;
		const size_t end = i + ilen;
		if (op == kOpCustomData) {
			i = end;
			continue;
		}
		if (declaration(op)) {
			if (op == kOpDclOutputSiv && ilen >= 4 && ((t[i + 1] >> 12) & 0xFFu) == kTypeOutput &&
				(t[i + 1] >> 31) == 0 && t[i + 3] == kNamePosition)
				out.pos = static_cast<int>(t[i + 2]);
			if (op == kOpDclConstantBuffer) {
				Operand o;
				const size_t used = operand(t, i + 1, end, o);
				if (used != 0 && o.type == kTypeConstantBuffer && o.dim == 3u && i + 1 + used + 2 <= end)
					out.spaces.emplace_back(o.index0, t[i + 1 + used + 1]);
			}
			i = end;
			continue;
		}
		if (ended || op == kOpLabel) {
			ended = true;
			i = end;
			continue;
		}
		switch (op) {
		case kOpIf: case kOpLoop: case kOpSwitch:
			++depth;
			i = end;
			continue;
		case kOpEndIf: case kOpEndLoop: case kOpEndSwitch:
			if (depth == 0)
				return false;
			--depth;
			i = end;
			continue;
		case kOpRet: case kOpRetC:
			if (depth != 0 || op == kOpRetC)
				return false;
			ended = true;
			i = end;
			continue;
		case kOpCall: case kOpCallC: case kOpInterfaceCall:
			return false;
		default:
			break;
		}
		size_t k = i + 1;
		for (bool ext = (tok >> 31) != 0; ext;) {
			if (k >= end)
				return false;
			ext = (t[k] >> 31) != 0;
			++k;
		}
		Instr in;
		in.op = op;
		in.once = depth == 0;
		in.saturate = (tok & kSaturate) != 0;
		const uint32_t writes = destinations(op);
		for (uint32_t index = 0; k < end; ++index) {
			Operand o;
			const size_t used = operand(t, k, end, o);
			if (used == 0)
				return false;
			k += used;
			if (index < writes) {
				if (in.dsts == 2u)
					return false;
				if (o.type == kTypeOutput && (o.relative || o.dim != 1u))
					return false;
				in.dst[in.dsts++] = o;
			} else if (in.srcs < 3u) {
				in.src[in.srcs++] = o;
			} else {
				in.more = true;
			}
		}
		if (k != end)
			return false;
		if (in.dsts != 0)
			out.code.push_back(in);
		i = end;
	}
	return i == len && out.pos >= 0;
}

bool constant(const Operand &o) noexcept
{
	return o.type == kTypeConstantBuffer || o.type == kTypeImm32 || o.type == kTypeImm64 ||
		o.type == kTypeImmediateConstantBuffer;
}

struct Tracer {
	const Shader &s;
	DxbcWTerms &out;
	int budget = kBudget;

	bool name(const Operand &k, uint8_t lane) noexcept
	{
		if (k.type != kTypeConstantBuffer)
			return false;
		uint32_t slot = 0, reg = 0, space = 0;
		if (k.dim == 2u && k.index0 < kD3D10OffsetSlots) {
			slot = k.index0;
			reg = k.index1;
		} else if (k.dim == 3u) {
			bool declared = false;
			for (const auto &sp : s.spaces) {
				if (sp.first == k.index0) {
					space = sp.second;
					declared = true;
				}
			}
			if (!declared)
				return false;
			slot = k.index1;
			reg = k.index2;
		} else {
			return false;
		}
		if (reg >= 4096u)
			return false;
		for (uint32_t i = 0; i < out.count; ++i) {
			if (out.slot[i] == slot && out.space[i] == space && out.reg[i] == reg && out.lane[i] == lane)
				return true;
		}
		if (out.count == DxbcWTerms::kMax)
			return false;
		out.slot[out.count] = slot;
		out.space[out.count] = space;
		out.reg[out.count] = static_cast<uint16_t>(reg);
		out.lane[out.count] = lane;
		++out.count;
		return true;
	}

	bool product(const Operand &a, uint8_t la, const Operand &b, uint8_t lb) noexcept
	{
		if (a.modified || b.modified)
			return false;
		const bool ka = constant(a), kb = constant(b);
		if (ka && kb)
			return !a.relative && !b.relative;
		if (ka == kb)
			return false;
		const Operand &k = ka ? a : b;
		const Operand &v = ka ? b : a;
		const uint8_t lk = ka ? la : lb, lv = ka ? lb : la;
		if (v.type != kTypeTemp && v.type != kTypeInput)
			return false;
		if (k.relative)
			return false;
		if (lv == 3u)
			return true;
		return name(k, lk);
	}

	bool addend(const Operand &o, uint8_t lane, size_t before) noexcept
	{
		if (o.modified)
			return false;
		if (constant(o))
			return !o.relative;
		if (o.type == kTypeInput)
			return lane == 3u;
		if (o.type == kTypeTemp && !o.relative)
			return value(kTypeTemp, o.index0, lane, before);
		return false;
	}

	bool value(uint32_t type, uint32_t num, uint8_t lane, size_t before) noexcept
	{
		if (--budget < 0)
			return false;
		for (size_t k = before; k-- > 0;) {
			const Instr &in = s.code[k];
			bool hit = false, first = false;
			for (uint32_t d = 0; d < in.dsts; ++d) {
				const Operand &o = in.dst[d];
				if (o.type == type && o.index0 == num && (o.mask & (1u << lane)) != 0) {
					hit = true;
					first = d == 0u;
				}
			}
			if (!hit)
				continue;
			if (!first || !in.once || in.saturate || in.more || in.dsts != 1u)
				return false;
			return written(k, lane);
		}
		return false;
	}

	bool written(size_t k, uint8_t lane) noexcept
	{
		const Instr &in = s.code[k];
		const auto at = [&in](uint32_t i, uint8_t l) { return in.src[i].swizzle[l]; };
		switch (in.op) {
		case kOpMov:
			return in.srcs == 1u && addend(in.src[0], at(0, lane), k);
		case kOpAdd:
			return in.srcs == 2u && addend(in.src[0], at(0, lane), k) && addend(in.src[1], at(1, lane), k);
		case kOpMul:
			return in.srcs == 2u && product(in.src[0], at(0, lane), in.src[1], at(1, lane));
		case kOpMad:
			return in.srcs == 3u && product(in.src[0], at(0, lane), in.src[1], at(1, lane)) &&
				addend(in.src[2], at(2, lane), k);
		case kOpDp2: case kOpDp3: case kOpDp4: {
			if (in.srcs != 2u)
				return false;
			const uint8_t dots = static_cast<uint8_t>(in.op == kOpDp2 ? 2u : in.op == kOpDp3 ? 3u : 4u);
			for (uint8_t i = 0; i < dots; ++i) {
				if (!product(in.src[0], at(0, i), in.src[1], at(1, i)))
					return false;
			}
			return true;
		}
		default:
			return false;
		}
	}
};

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

JitterVertexClass classify_d3d9_vertex_shader(const uint32_t *code, size_t dwords)
{
	vclass::Program p;
	if (!vclass::read_d3d9(code, dwords, p))
		return JitterVertexClass::Unknown;
	return vclass::weigh(p);
}

JitterVertexClass classify_dxbc_vertex_shader(const void *code, size_t bytes)
{
	std::vector<uint32_t> t;
	size_t len = 0;
	if (!dxbc_program(code, bytes, t, len))
		return JitterVertexClass::Unknown;
	vclass::Program p;
	if (!vclass::read_sm4(t.data(), len, p))
		return JitterVertexClass::Unknown;
	return vclass::weigh(p);
}

bool d3d9_position_w_terms(const uint32_t *code, size_t dwords, D3D9WTerms *out)
{
	if (out == nullptr)
		return false;
	*out = D3D9WTerms{};
	wterms::Shader s;
	if (!wterms::read(code, dwords, s))
		return false;
	wterms::Tracer t{ s, *out };
	if (!t.value(s.pos_type, s.pos_num, 3u, s.code.size())) {
		*out = D3D9WTerms{};
		return false;
	}
	return true;
}

bool d3d9_w_terms_perspective(const D3D9WTerms &terms, const float *constants, uint32_t registers) noexcept
{
	if (constants == nullptr)
		return false;
	for (uint32_t i = 0; i < terms.count && i < D3D9WTerms::kMax; ++i) {
		if (terms.reg[i] >= registers || terms.lane[i] > 3u)
			continue;
		const float v = constants[terms.reg[i] * 4u + terms.lane[i]];
		if (v > 1e-6f || v < -1e-6f)
			return true;
	}
	return false;
}

bool dxbc_position_w_terms(const void *code, size_t bytes, DxbcWTerms *out)
{
	if (out == nullptr)
		return false;
	*out = DxbcWTerms{};
	std::vector<uint32_t> t;
	size_t len = 0;
	if (!dxbc_program(code, bytes, t, len))
		return false;
	wterms4::Shader s;
	if (!wterms4::read(t.data(), len, s))
		return false;
	wterms4::Tracer tracer{ s, *out };
	if (!tracer.value(wterms4::kTypeOutput, static_cast<uint32_t>(s.pos), 3u, s.code.size())) {
		*out = DxbcWTerms{};
		return false;
	}
	return true;
}

int dxbc_w_terms_perspective(const DxbcWTerms &terms, DxbcConstantRead read, void *user) noexcept
{
	if (read == nullptr)
		return -1;
	struct Unspaced {
		DxbcConstantRead read;
		void *user;
	} u{ read, user };
	return dxbc_w_terms_perspective_spaced(
		terms,
		[](void *p, uint32_t space, uint32_t slot, uint32_t reg, float out[4]) {
			const Unspaced &x = *static_cast<const Unspaced *>(p);
			return space == 0u && x.read(x.user, slot, reg, out);
		},
		&u);
}

int dxbc_w_terms_perspective_spaced(const DxbcWTerms &terms, DxbcSpacedConstantRead read, void *user) noexcept
{
	if (read == nullptr)
		return -1;
	bool unknown = false;
	for (uint32_t i = 0; i < terms.count && i < DxbcWTerms::kMax; ++i) {
		float v[4] = {};
		if (terms.lane[i] > 3u || !read(user, terms.space[i], terms.slot[i], terms.reg[i], v)) {
			unknown = true;
			continue;
		}
		const float x = v[terms.lane[i]];
		if (x != x) {
			unknown = true;
			continue;
		}
		if (x > 1e-6f || x < -1e-6f)
			return 1;
	}
	return unknown ? -1 : 0;
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

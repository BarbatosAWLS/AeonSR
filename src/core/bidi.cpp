#include "aeon_sr/core/bidi.hpp"

#include <algorithm>
#include <vector>

namespace aeon_sr {
namespace bidi {
namespace {

struct Forms {
	unsigned short isolated, final, initial, medial;
};
const Forms kForms[] = {
	{ 0xFE80, 0, 0, 0 },
	{ 0xFE81, 0xFE82, 0, 0 },
	{ 0xFE83, 0xFE84, 0, 0 },
	{ 0xFE85, 0xFE86, 0, 0 },
	{ 0xFE87, 0xFE88, 0, 0 },
	{ 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C },
	{ 0xFE8D, 0xFE8E, 0, 0 },
	{ 0xFE8F, 0xFE90, 0xFE91, 0xFE92 },
	{ 0xFE93, 0xFE94, 0, 0 },
	{ 0xFE95, 0xFE96, 0xFE97, 0xFE98 },
	{ 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C },
	{ 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0 },
	{ 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4 },
	{ 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8 },
	{ 0xFEA9, 0xFEAA, 0, 0 },
	{ 0xFEAB, 0xFEAC, 0, 0 },
	{ 0xFEAD, 0xFEAE, 0, 0 },
	{ 0xFEAF, 0xFEB0, 0, 0 },
	{ 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4 },
	{ 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8 },
	{ 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC },
	{ 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0 },
	{ 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4 },
	{ 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8 },
	{ 0xFEC9, 0xFECA, 0xFECB, 0xFECC },
	{ 0xFECD, 0xFECE, 0xFECF, 0xFED0 },
	{ 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
	{ 0x0640, 0x0640, 0x0640, 0x0640 },
	{ 0xFED1, 0xFED2, 0xFED3, 0xFED4 },
	{ 0xFED5, 0xFED6, 0xFED7, 0xFED8 },
	{ 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC },
	{ 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0 },
	{ 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4 },
	{ 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8 },
	{ 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC },
	{ 0xFEED, 0xFEEE, 0, 0 },
	{ 0xFEEF, 0xFEF0, 0, 0 },
	{ 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4 },
};

bool letter(unsigned int c) noexcept
{
	return c >= 0x0621 && c <= 0x064A && kForms[c - 0x0621].isolated != 0;
}

bool transparent(unsigned int c) noexcept
{
	return (c >= 0x064B && c <= 0x065F) || c == 0x0670;
}

const Forms &forms(unsigned int c) noexcept
{
	return kForms[c - 0x0621];
}

bool joins_forward(unsigned int c) noexcept
{
	return letter(c) && forms(c).initial != 0;
}

bool presentation(unsigned int c) noexcept
{
	return (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

bool arabic(unsigned int c) noexcept
{
	return (c >= 0x0600 && c <= 0x06FF) || presentation(c);
}

bool arabic_letter(unsigned int c) noexcept
{
	return letter(c) || (presentation(c) && c != 0xFEFF);
}

bool strong_ltr(unsigned int c) noexcept
{
	if (c < 0x80)
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
	return !arabic(c) && c >= 0xC0;
}

std::vector<unsigned int> decode(std::string_view s)
{
	std::vector<unsigned int> out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size();) {
		const unsigned char b = static_cast<unsigned char>(s[i]);
		unsigned int c = b, n = 0;
		if (b >= 0xF0) { c = b & 0x07u; n = 3; }
		else if (b >= 0xE0) { c = b & 0x0Fu; n = 2; }
		else if (b >= 0xC0) { c = b & 0x1Fu; n = 1; }
		++i;
		for (unsigned int k = 0; k < n && i < s.size(); ++k, ++i)
			c = (c << 6) | (static_cast<unsigned char>(s[i]) & 0x3Fu);
		out.push_back(c);
	}
	return out;
}

void encode(std::string &s, unsigned int c)
{
	if (c < 0x80) {
		s.push_back(static_cast<char>(c));
	} else if (c < 0x800) {
		s.push_back(static_cast<char>(0xC0 | (c >> 6)));
		s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
	} else if (c < 0x10000) {
		s.push_back(static_cast<char>(0xE0 | (c >> 12)));
		s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
		s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
	} else {
		s.push_back(static_cast<char>(0xF0 | (c >> 18)));
		s.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
		s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
		s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
	}
}

unsigned int mirrored(unsigned int c) noexcept
{
	switch (c) {
	case '(': return ')';
	case ')': return '(';
	case '[': return ']';
	case ']': return '[';
	case '{': return '}';
	case '}': return '{';
	case '<': return '>';
	case '>': return '<';
	default: return c;
	}
}

std::vector<unsigned int> shape(const std::vector<unsigned int> &w)
{
	std::vector<unsigned int> out;
	out.reserve(w.size());
	const auto neighbour = [&](size_t i, int step) -> unsigned int {
		for (long j = static_cast<long>(i) + step; j >= 0 && j < static_cast<long>(w.size()); j += step)
			if (!transparent(w[static_cast<size_t>(j)]))
				return w[static_cast<size_t>(j)];
		return 0;
	};
	for (size_t i = 0; i < w.size(); ++i) {
		const unsigned int c = w[i];
		if (!letter(c)) {
			out.push_back(c);
			continue;
		}
		const unsigned int before = neighbour(i, -1);
		const bool from_before = joins_forward(before);
		if (c == 0x0644) {
			size_t j = i + 1;
			while (j < w.size() && transparent(w[j]))
				++j;
			if (j < w.size()) {
				unsigned int lig = 0;
				switch (w[j]) {
				case 0x0622: lig = 0xFEF5; break;
				case 0x0623: lig = 0xFEF7; break;
				case 0x0625: lig = 0xFEF9; break;
				case 0x0627: lig = 0xFEFB; break;
				default: break;
				}
				if (lig != 0) {
					out.push_back(from_before ? lig + 1 : lig);
					for (size_t k = i + 1; k < j; ++k)
						out.push_back(w[k]);
					i = j;
					continue;
				}
			}
		}
		const unsigned int after = neighbour(i, 1);
		const bool to_after = joins_forward(c) && letter(after) && forms(after).final != 0;
		const Forms &f = forms(c);
		if (from_before && to_after && f.medial != 0)
			out.push_back(f.medial);
		else if (from_before && f.final != 0)
			out.push_back(f.final);
		else if (to_after && f.initial != 0)
			out.push_back(f.initial);
		else
			out.push_back(f.isolated);
	}
	return out;
}

std::vector<unsigned int> reversed(const std::vector<unsigned int> &w)
{
	std::vector<unsigned int> out;
	out.reserve(w.size());
	size_t end = w.size();
	while (end > 0) {
		size_t start = end - 1;
		while (start > 0 && transparent(w[start]))
			--start;
		out.push_back(mirrored(w[start]));
		for (size_t k = start + 1; k < end; ++k)
			out.push_back(w[k]);
		end = start;
	}
	return out;
}

enum class Dir { L, R, N };

struct Token {
	std::vector<unsigned int> cps;
	Dir dir = Dir::N;
	unsigned int spaces = 0;
	bool split = false;
};

bool neutral_edge(unsigned int c) noexcept
{
	return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '.' || c == ',' ||
		c == ':' || c == ';' || c == '!' || c == '?' || c == '"' || c == '\'' || c == 0x060C || c == 0x061B ||
		c == 0x061F || c == '-';
}

Dir classify(const std::vector<unsigned int> &w)
{
	for (unsigned int c : w)
		if (arabic_letter(c))
			return Dir::R;
	for (unsigned int c : w)
		if (strong_ltr(c))
			return Dir::L;
	return Dir::N;
}

std::vector<Token> tokens(const std::vector<unsigned int> &line, unsigned int *lead, unsigned int *trail)
{
	std::vector<Token> out;
	unsigned int space = 0;
	*lead = *trail = 0;
	for (size_t i = 0; i < line.size();) {
		if (line[i] == ' ') {
			++space;
			++i;
			continue;
		}
		if (out.empty())
			*lead = space;
		size_t e = i;
		while (e < line.size() && line[e] != ' ')
			++e;
		std::vector<unsigned int> word(line.begin() + static_cast<long>(i), line.begin() + static_cast<long>(e));
		size_t a = 0, z = word.size();
		while (a < z && neutral_edge(word[a]))
			++a;
		while (z > a && neutral_edge(word[z - 1]))
			--z;
		const bool cut = a < z && (a > 0 || z < word.size()) && classify(std::vector<unsigned int>(
			word.begin() + static_cast<long>(a), word.begin() + static_cast<long>(z))) == Dir::L;
		if (!cut) {
			Token t;
			t.cps = std::move(word);
			t.dir = classify(t.cps);
			t.spaces = out.empty() ? 0 : space;
			out.push_back(std::move(t));
		} else {
			bool first = true;
			for (unsigned int k = 0; k < a; ++k) {
				Token t;
				t.cps = { word[k] };
				t.split = true;
				t.spaces = first && !out.empty() ? space : 0;
				first = false;
				out.push_back(std::move(t));
			}
			Token core;
			core.cps.assign(word.begin() + static_cast<long>(a), word.begin() + static_cast<long>(z));
			core.dir = Dir::L;
			core.spaces = first && !out.empty() ? space : 0;
			out.push_back(std::move(core));
			for (size_t k = z; k < word.size(); ++k) {
				Token t;
				t.cps = { word[k] };
				t.split = true;
				out.push_back(std::move(t));
			}
		}
		space = 0;
		i = e;
	}
	*trail = space;
	return out;
}

std::string visual_line(const std::vector<unsigned int> &line, int paragraph)
{
	unsigned int lead = 0, trail = 0;
	std::vector<Token> t = tokens(line, &lead, &trail);
	if (t.empty())
		return std::string(lead + trail, ' ');
	Dir para = Dir::L;
	for (const Token &k : t)
		if (k.dir == Dir::R)
			para = Dir::R;
	if (paragraph != 0)
		para = paragraph > 0 ? Dir::R : Dir::L;
	for (size_t i = 0; i < t.size(); ++i) {
		if (t[i].dir != Dir::N)
			continue;
		size_t j = i;
		while (j < t.size() && t[j].dir == Dir::N)
			++j;
		const Dir before = i > 0 ? t[i - 1].dir : para;
		const Dir after = j < t.size() ? t[j].dir : para;
		const Dir d = before == after ? before : para;
		for (size_t k = i; k < j; ++k)
			t[k].dir = d;
		i = j - 1;
	}
	for (Token &k : t)
		if (k.dir == Dir::R && (std::any_of(k.cps.begin(), k.cps.end(), letter) ||
				std::none_of(k.cps.begin(), k.cps.end(), presentation)))
			k.cps = reversed(shape(k.cps));

	struct Run {
		size_t first, last;
		Dir dir;
	};
	std::vector<Run> runs;
	for (size_t i = 0; i < t.size();) {
		size_t j = i;
		while (j + 1 < t.size() && t[j + 1].dir == t[i].dir)
			++j;
		runs.push_back({ i, j, t[i].dir });
		i = j + 1;
	}
	if (para == Dir::R)
		std::reverse(runs.begin(), runs.end());
	std::vector<size_t> order;
	for (const Run &r : runs) {
		if (r.dir == Dir::R)
			for (size_t k = r.last + 1; k-- > r.first;)
				order.push_back(k);
		else
			for (size_t k = r.first; k <= r.last; ++k)
				order.push_back(k);
	}
	std::string out(para == Dir::R ? trail : lead, ' ');
	for (size_t n = 0; n < order.size(); ++n) {
		if (n > 0) {
			const size_t a = order[n - 1], b = order[n];
			unsigned int spaces;
			if (a + 1 == b)
				spaces = t[b].spaces;
			else if (b + 1 == a)
				spaces = t[a].spaces;
			else
				spaces = !t[a].split && !t[b].split ? 1u : 0u;
			out.append(spaces, ' ');
		}
		for (unsigned int c : t[order[n]].cps)
			encode(out, c);
	}
	out.append(para == Dir::R ? lead : trail, ' ');
	return out;
}

}

bool needs_shaping(std::string_view text) noexcept
{
	for (size_t i = 0; i + 1 < text.size(); ++i) {
		const unsigned char b = static_cast<unsigned char>(text[i]);
		if (b >= 0xD8 && b <= 0xD9) {
			const unsigned int c = ((b & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
			if (letter(c))
				return true;
		}
	}
	return false;
}

int direction(std::string_view text) noexcept
{
	int d = 0;
	for (unsigned int c : decode(text)) {
		if (arabic_letter(c))
			return 1;
		if (strong_ltr(c))
			d = -1;
	}
	return d;
}

std::string visual(std::string_view text, int paragraph)
{
	if (!needs_shaping(text) && paragraph <= 0)
		return std::string(text);
	const size_t id = text.find("##");
	const std::string_view shown = text.substr(0, id);
	std::string out;
	size_t start = 0;
	while (start <= shown.size()) {
		size_t nl = shown.find('\n', start);
		if (nl == std::string_view::npos)
			nl = shown.size();
		out += visual_line(decode(shown.substr(start, nl - start)), paragraph);
		if (nl == shown.size())
			break;
		out.push_back('\n');
		start = nl + 1;
	}
	if (id != std::string_view::npos)
		out.append(text.substr(id));
	return out;
}

}
}

#include "aeon_sr/core/panel_widgets.hpp"
#include "aeon_sr/core/bidi.hpp"
#include "aeon_sr/core/i18n.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace aeon_sr {
namespace ui {
namespace {

Palette g_pal;
bool g_icons = false;
bool g_dark = true;
int g_dim = 0;
int g_tip_colours = 0;

struct Card {
	ImVec2 min;
	float right;
	float pad;
	ImGuiID height_id;
	ImGuiID stack_id = 0;
	bool stack = false;
	bool stack_next = false;
};
constexpr int kMaxCards = 4;
Card g_cards[kMaxCards];
int g_card_depth = 0;

float g_caption_fit = 0.0f;
float g_caption_next = 0.0f;
ImGuiID g_caption_fit_id = 0;
float g_value_fit = 0.0f;
float g_value_next = 0.0f;
ImGuiID g_value_fit_id = 0;
constexpr float kCaptionChars = 17.0f;
float g_title_fit = 0.0f;
float g_title_next = 0.0f;
ImGuiID g_title_fit_id = 0;
bool g_title_caps = true;
bool g_title_caps_next = true;
ImGuiID g_title_caps_id = 0;

float unit()
{
	return ImGui::GetFontSize() / 13.0f;
}

ImU32 col(const ImVec4 &c)
{
	return ImGui::GetColorU32(c);
}

constexpr float kDimAlpha = 0.45f;

ImVec4 dim_text()
{
	return ImVec4(g_pal.faded.x, g_pal.faded.y, g_pal.faded.z, 1.0f / kDimAlpha);
}

ImVec4 undimmed(const ImVec4 &c)
{
	return ImVec4(c.x, c.y, c.z, 1.0f / kDimAlpha);
}

ImVec4 ink(const ImVec4 &c)
{
	return g_dim > 0 ? dim_text() : c;
}

float card_pad()
{
	return 10.0f * unit();
}

float hairline()
{
	return std::max(1.0f, std::floor(unit()));
}

float icon_drop()
{
	return std::round(1.5f * unit());
}

void block_gap(float bottom)
{
	const float x = ImGui::GetCursorScreenPos().x;
	ImGui::SetCursorScreenPos(ImVec2(x, bottom + 7.0f * unit() - ImGui::GetStyle().ItemSpacing.y));
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

const char *visible_end(const char *s)
{
	while (*s != '\0' && !(s[0] == '#' && s[1] == '#'))
		++s;
	return s;
}

bool same(const ImVec4 &a, const ImVec4 &b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

void gradient_rect(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, const ImVec4 &left, const ImVec4 &right,
	float rounding, ImDrawFlags corners)
{
	const float w = b.x - a.x;
	if (w <= 0.0f)
		return;
	if (w <= rounding * 2.0f + 1.0f) {
		dl->AddRectFilled(a, b, col(mix(left, right, 0.5f)), rounding, corners);
		return;
	}
	const float mid = a.x + w * 0.5f;
	dl->PushClipRect(a, ImVec2(mid, b.y), true);
	dl->AddRectFilled(a, b, col(left), rounding, corners);
	dl->PopClipRect();
	dl->PushClipRect(ImVec2(mid, a.y), b, true);
	dl->AddRectFilled(a, b, col(right), rounding, corners);
	dl->PopClipRect();
	dl->AddRectFilledMultiColor(ImVec2(a.x + rounding, a.y), ImVec2(b.x - rounding, b.y),
		col(left), col(right), col(right), col(left));
}

void shade(ImDrawList *dl, int first, const ImVec2 &p0, const ImVec2 &p1, const ImVec4 &a, const ImVec4 &b)
{
	const ImVec2 d(p1.x - p0.x, p1.y - p0.y);
	const float len2 = d.x * d.x + d.y * d.y;
	for (int i = first; i < dl->VtxBuffer.Size; ++i) {
		ImDrawVert &v = dl->VtxBuffer.Data[i];
		const float t = len2 > 0.0f
			? std::clamp(((v.pos.x - p0.x) * d.x + (v.pos.y - p0.y) * d.y) / len2, 0.0f, 1.0f) : 0.0f;
		const ImVec4 c = mix(a, b, t);
		const auto byte = [](float f) { return static_cast<ImU32>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
		v.col = (v.col & IM_COL32_A_MASK) | IM_COL32(byte(c.x), byte(c.y), byte(c.z), 0);
	}
}

unsigned int code_point(const char *p, int *len)
{
	const unsigned char *u = reinterpret_cast<const unsigned char *>(p);
	if (u[0] < 0x80 || (u[0] & 0xC0) == 0x80) {
		*len = 1;
		return u[0];
	}
	const int n = (u[0] & 0xE0) == 0xC0 ? 2 : (u[0] & 0xF0) == 0xE0 ? 3 : 4;
	unsigned int c = u[0] & (n == 2 ? 0x1Fu : n == 3 ? 0x0Fu : 0x07u);
	for (int i = 1; i < n; ++i) {
		if ((u[i] & 0xC0) != 0x80) {
			*len = i;
			return 0xFFFD;
		}
		c = (c << 6) | (u[i] & 0x3Fu);
	}
	*len = n;
	return c;
}

void append_utf8(std::string &s, unsigned int c)
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

bool combining(unsigned int c)
{
	return (c >= 0x0300 && c <= 0x036F) || c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E3A) ||
		(c >= 0x0E47 && c <= 0x0E4E) || (c >= 0x064B && c <= 0x065F) || c == 0x0670;
}

bool breaks_after(unsigned int c)
{
	return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
		(c >= 0xFF00 && c <= 0xFFEF);
}

bool stays_with_previous(unsigned int c)
{
	if (combining(c))
		return true;
	switch (c) {
	case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B: case 0xFF01: case 0xFF1F:
	case 0xFF09: case 0xFF3D: case 0xFF5D: case 0x300D: case 0x300F: case 0x3011: case 0x3009: case 0x300B:
	case 0x3015: case 0x30FC: case 0x3005: case 0x309D: case 0x309E: case 0x30FB:
	case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063: case 0x3083: case 0x3085:
	case 0x3087: case 0x308E: case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9: case 0x30C3:
	case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE: case 0x30F5: case 0x30F6:
	case '.': case ',': case ':': case ';': case '!': case '?': case ')': case ']': case '}': case '%':
		return true;
	default:
		return false;
	}
}

bool stays_with_next(unsigned int c)
{
	return c == 0xFF08 || c == 0x300C || c == 0x300E || c == 0x3010 || c == 0x3008 || c == 0x300A ||
		c == 0x3014 || c == 0xFF3B || c == 0xFF5B || c == '(' || c == '[';
}

unsigned int capital(unsigned int c)
{
	const bool turkish = i18n::current() == i18n::Lang::tr_TR;
	if (c >= 'a' && c <= 'z')
		return turkish && c == 'i' ? 0x0130u : c - 0x20u;
	if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
		return c - 0x20u;
	if (c == 0x0131)
		return 'I';
	if ((c >= 0x0100 && c <= 0x0137) || (c >= 0x014A && c <= 0x0177))
		return c & ~1u;
	if ((c >= 0x0139 && c <= 0x0148) || (c >= 0x0179 && c <= 0x017E))
		return (c & 1u) == 0 ? c - 1u : c;
	if (c >= 0x0430 && c <= 0x044F)
		return c - 0x20u;
	if (c >= 0x0450 && c <= 0x045F)
		return c - 0x50u;
	return c;
}

std::string upper_case(const char *s)
{
	std::string out;
	for (const char *p = s; *p != '\0';) {
		int n = 1;
		const unsigned int c = code_point(p, &n);
		append_utf8(out, capital(c));
		p += n;
	}
	return out;
}

const char *cluster_end(const char *p)
{
	int n = 1;
	code_point(p, &n);
	p += n;
	while (*p != '\0') {
		const unsigned int c = code_point(p, &n);
		if (!combining(c))
			break;
		p += n;
	}
	return p;
}

float tracked_width(const char *s, float tracking)
{
	if (bidi::needs_shaping(s))
		return ImGui::CalcTextSize(s).x;
	float w = 0.0f;
	for (const char *c = s; *c != '\0';) {
		const char *e = cluster_end(c);
		w += ImGui::CalcTextSize(c, e).x + (*e != '\0' ? tracking : 0.0f);
		c = e;
	}
	return w;
}

float tracked_text(ImDrawList *dl, float x, float y, const ImVec4 &c, const char *s, float tracking)
{
	if (bidi::needs_shaping(s)) {
		dl->AddText(ImVec2(x, y), col(c), s);
		return x + ImGui::CalcTextSize(s).x;
	}
	for (const char *ch = s; *ch != '\0';) {
		const char *e = cluster_end(ch);
		dl->AddText(ImVec2(x, y), col(c), ch, e);
		x += ImGui::CalcTextSize(ch, e).x + tracking;
		ch = e;
	}
	return x;
}

float title_tracking()
{
	return std::floor(unit());
}

void chevron(ImDrawList *dl, const ImVec2 &centre, bool open, const ImVec4 &c)
{
	if (g_icons) {
		const char *g = open ? icon::kChevronDown : icon::kChevronRight;
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
		const ImVec2 s = ImGui::CalcTextSize(g);
		dl->AddText(ImVec2(centre.x - s.x * 0.5f, centre.y - s.y * 0.5f), col(c), g);
		ImGui::PopFont();
		return;
	}
	const float r = 3.5f * unit();
	if (open)
		dl->AddTriangleFilled(ImVec2(centre.x - r, centre.y - r * 0.5f), ImVec2(centre.x + r, centre.y - r * 0.5f),
			ImVec2(centre.x, centre.y + r * 0.6f), col(c));
	else
		dl->AddTriangleFilled(ImVec2(centre.x - r * 0.5f, centre.y - r), ImVec2(centre.x + r * 0.6f, centre.y),
			ImVec2(centre.x - r * 0.5f, centre.y + r), col(c));
}

struct Piece {
	const char *b, *e;
	int span;
	float x;
};
struct TextLine {
	int first, count;
	float w;
};
struct Layout {
	std::vector<Piece> pieces;
	std::vector<TextLine> lines;
	float w = 0.0f;
	bool rtl = false;
};

float piece_width(const char *b, const char *e, bool rtl)
{
	if (!rtl)
		return ImGui::CalcTextSize(b, e).x;
	const std::string v = bidi::visual(std::string_view(b, static_cast<size_t>(e - b)), 1);
	return ImGui::CalcTextSize(v.c_str(), v.c_str() + v.size()).x;
}

bool soft_break_after(char c)
{
	return c == '_' || c == '.' || c == '/' || c == '\\' || c == '-' || c == ',' || c == ':';
}

void lay_out(const Span *spans, int count, float width, Layout &out, float align = 0.0f)
{
	struct Unit {
		int first, count;
		bool space, brk;
	};
	std::vector<Piece> src;
	std::vector<Unit> units;
	bool space = false, brk = false, cjk = false;
	int dir = 0;
	for (int s = 0; s < count && dir == 0; ++s)
		if (spans[s].text != nullptr)
			dir = bidi::direction(spans[s].text);
	const bool rtl = dir > 0;
	for (int s = 0; s < count; ++s) {
		const char *p = spans[s].text;
		while (p != nullptr && *p != '\0') {
			if (*p == '\n') {
				brk = true;
				space = false;
				++p;
				continue;
			}
			if (*p == ' ') {
				space = true;
				++p;
				continue;
			}
			const char *b = p;
			bool split = false;
			while (*p != '\0' && *p != ' ' && *p != '\n') {
				int n = 1;
				const unsigned int c = code_point(p, &n);
				p += n;
				if (breaks_after(c) && !stays_with_next(c) && *p != '\0' && *p != ' ' && *p != '\n' &&
					!stays_with_previous(code_point(p, &n))) {
					split = true;
					break;
				}
			}
			const bool dash = p - b == 1 && *b == '-';
			if (!units.empty() && !brk && space && dash && src.back().span == s) {
				src.back().e = p;
			} else if (!units.empty() && !brk && !space && !cjk) {
				src.push_back(Piece{ b, p, s, 0.0f });
				++units.back().count;
			} else {
				units.push_back(Unit{ static_cast<int>(src.size()), 1, space, brk });
				src.push_back(Piece{ b, p, s, 0.0f });
			}
			space = brk = false;
			cjk = split;
		}
		cjk = false;
	}

	out.pieces.clear();
	out.lines.clear();
	out.w = 0.0f;
	out.rtl = rtl;
	out.lines.push_back(TextLine{ 0, 0, 0.0f });
	const float space_w = ImGui::CalcTextSize(" ").x;
	float x = 0.0f;
	const auto new_line = [&]() {
		out.w = std::max(out.w, out.lines.back().w);
		out.lines.push_back(TextLine{ static_cast<int>(out.pieces.size()), 0, 0.0f });
		x = 0.0f;
	};
	const auto place = [&](const char *b, const char *e, int span, float at, float w) {
		out.pieces.push_back(Piece{ b, e, span, at });
		++out.lines.back().count;
		x = at + w;
		out.lines.back().w = x;
	};
	for (const Unit &u : units) {
		float w = 0.0f;
		for (int i = 0; i < u.count; ++i)
			w += piece_width(src[static_cast<size_t>(u.first + i)].b, src[static_cast<size_t>(u.first + i)].e, rtl);
		bool empty = out.lines.back().count == 0;
		if (u.brk && !empty) {
			new_line();
			empty = true;
		}
		float gap = !empty && u.space ? space_w : 0.0f;
		if (!empty && x + gap + w > width) {
			new_line();
			empty = true;
			gap = 0.0f;
		}
		if (x + gap + w <= width || !empty) {
			float at = x + gap;
			for (int i = 0; i < u.count; ++i) {
				const Piece &p = src[static_cast<size_t>(u.first + i)];
				const float pw = piece_width(p.b, p.e, rtl);
				place(p.b, p.e, p.span, at, pw);
				at += pw;
			}
			continue;
		}
		for (int i = 0; i < u.count; ++i) {
			const Piece &p = src[static_cast<size_t>(u.first + i)];
			const char *b = p.b;
			while (b < p.e) {
				const float room = width - x;
				const char *cut = nullptr, *soft = nullptr;
				for (const char *c = b + 1; c <= p.e; ++c) {
					if (c < p.e && (static_cast<unsigned char>(*c) & 0xC0) == 0x80)
						continue;
					int n = 1;
					if (c < p.e && stays_with_previous(code_point(c, &n)))
						continue;
					if (piece_width(b, c, rtl) > room)
						break;
					cut = c;
					if (c < p.e && soft_break_after(c[-1]))
						soft = c;
				}
				if (cut == p.e) {
					place(b, p.e, p.span, x, piece_width(b, p.e, rtl));
					break;
				}
				const char *at = soft != nullptr ? soft : cut;
				if (at == nullptr) {
					if (x > 0.0f) {
						new_line();
						continue;
					}
					at = cluster_end(b);
					if (at > p.e)
						at = p.e;
				}
				place(b, at, p.span, x, piece_width(b, at, rtl));
				new_line();
				b = at;
			}
		}
	}
	out.w = std::max(out.w, out.lines.back().w);
	if (!rtl)
		return;
	const float edge = align > 0.0f ? align : width < 1.0e8f ? width : out.w;
	const auto ltr = [](const Piece &q) {
		return bidi::direction(std::string_view(q.b, static_cast<size_t>(q.e - q.b))) < 0;
	};
	std::vector<Piece> pieces;
	pieces.reserve(out.pieces.size());
	for (TextLine &line : out.lines) {
		const int first = static_cast<int>(pieces.size());
		for (int i = 0; i < line.count;) {
			Piece p = out.pieces[static_cast<size_t>(line.first + i)];
			int j = i;
			if (ltr(p))
				while (j + 1 < line.count) {
					const Piece &q = out.pieces[static_cast<size_t>(line.first + j + 1)];
					if (q.span != p.span || !ltr(q))
						break;
					++j;
				}
			p.e = out.pieces[static_cast<size_t>(line.first + j)].e;
			p.x = edge - (p.x + piece_width(p.b, p.e, true));
			pieces.push_back(p);
			i = j + 1;
		}
		line.first = first;
		line.count = static_cast<int>(pieces.size()) - first;
	}
	out.pieces = std::move(pieces);
	out.w = std::max(out.w, edge);
}

void lay_out_paragraph(const Span *spans, int count, float width, Layout &out)
{
	lay_out(spans, count, width, out);
	for (int s = 0; s < count; ++s)
		if (spans[s].text != nullptr && std::strchr(spans[s].text, '\n') != nullptr)
			return;
	const size_t n = out.lines.size();
	if (n < 2 || out.lines[n - 2].count < 3 || out.lines[n - 1].count != 1)
		return;
	Layout tighter;
	lay_out(spans, count, out.lines[n - 2].w - 1.0f, tighter, width < 1.0e8f ? width : 0.0f);
	if (tighter.lines.size() == n && tighter.lines[n - 1].count > 1)
		out = tighter;
}

void draw_layout(ImDrawList *dl, const Layout &l, const ImVec2 &origin, const Span *spans, float line_h)
{
	for (size_t li = 0; li < l.lines.size(); ++li) {
		const TextLine &line = l.lines[li];
		const float y = origin.y + static_cast<float>(li) * line_h;
		for (int i = 0; i < line.count; ++i) {
			const Piece &p = l.pieces[static_cast<size_t>(line.first + i)];
			if (l.rtl) {
				const std::string v = bidi::visual(std::string_view(p.b, static_cast<size_t>(p.e - p.b)), 1);
				dl->AddText(ImVec2(origin.x + p.x, y), col(ink(spans[p.span].colour)), v.c_str(), v.c_str() + v.size());
				continue;
			}
			dl->AddText(ImVec2(origin.x + p.x, y), col(ink(spans[p.span].colour)), p.b, p.e);
		}
	}
}

float text_room()
{
	return std::max(1.0f, std::min(avail_width(), ImGui::GetContentRegionAvail().x));
}

constexpr float kPillFill = 0.13f;
constexpr float kPillEdge = 0.38f;
constexpr float kCalmFill = 0.065f;
constexpr float kCalmEdge = 0.17f;

float pill_lead()
{
	return (g_icons ? 22.0f : 20.0f) * unit();
}

float pill_trail()
{
	return 9.0f * unit();
}

float pill_line()
{
	return ImGui::GetFontSize() + 6.0f * unit();
}

bool alert(const ImVec4 &c)
{
	return same(c, g_pal.warn) || same(c, g_pal.fail);
}

ImVec4 pill_text_colour(const ImVec4 &c)
{
	if (alert(c))
		return mix(c, g_pal.text, 0.3f);
	return same(c, g_pal.off) ? g_pal.label : g_pal.text;
}

const char *pill_caption(const char *text, char (&buf)[512])
{
	int n = 1;
	const unsigned int c = code_point(text, &n);
	std::string first;
	if (*text != '\0')
		append_utf8(first, capital(c));
	std::snprintf(buf, sizeof(buf), "%s%s", first.c_str(), *text != '\0' ? text + n : "");
	return buf;
}

float pill_wrap(const char *caption, float max_w)
{
	const Span span{ caption, g_pal.text };
	const float room = std::max(1.0f, max_w - pill_lead() - pill_trail());
	Layout l;
	lay_out(&span, 1, room, l);
	const size_t lines = l.lines.size();
	float lo = 1.0f, hi = room;
	for (int i = 0; i < 14 && hi - lo > 1.0f; ++i) {
		const float mid = (lo + hi) * 0.5f;
		lay_out(&span, 1, mid, l);
		(l.lines.size() <= lines ? hi : lo) = mid;
	}
	return hi;
}

ImVec2 pill_size(const char *text, float max_w = 0.0f)
{
	char buf[512];
	const char *t = pill_caption(text, buf);
	ImVec2 s = ImGui::CalcTextSize(t);
	if (max_w > 0.0f) {
		const Span span{ t, g_pal.text };
		Layout l;
		lay_out(&span, 1, pill_wrap(t, max_w), l);
		s = ImVec2(l.w, ImGui::GetTextLineHeight() * static_cast<float>(l.lines.size()));
	}
	return ImVec2(pill_lead() + s.x + pill_trail(), s.y + 6.0f * unit());
}

const char *state_glyph(const ImVec4 &colour)
{
	if (!g_icons)
		return nullptr;
	return same(colour, g_pal.ok) ? icon::kOk : same(colour, g_pal.warn) ? icon::kWarn
		: same(colour, g_pal.fail) ? icon::kFail : nullptr;
}

void status_dot(ImDrawList *dl, const ImVec2 &c, const ImVec4 &colour)
{
	const float k = unit();
	if (const char *g = state_glyph(colour)) {
		const ImVec2 s = ImGui::CalcTextSize(g);
		dl->AddText(ImVec2(std::floor(c.x - s.x * 0.5f), std::floor(c.y - s.y * 0.5f) + icon_drop()), col(colour), g);
		return;
	}
	const float dot = 3.0f * k;
	if (same(colour, g_pal.off)) {
		dl->AddCircle(c, dot, col(colour), 12, std::max(1.0f, std::floor(1.5f * k)));
		return;
	}
	dl->AddCircleFilled(c, dot, col(colour), 12);
}

void pill_at(ImDrawList *dl, const ImVec2 &p, const ImVec4 &colour, const char *text, float max_w = 0.0f)
{
	const float k = unit();
	char buf[512];
	const char *t = pill_caption(text, buf);
	const ImVec2 s = pill_size(text, max_w);
	const float dot = 3.0f * k;
	const float r = pill_line() * 0.5f;
	const ImVec2 q(p.x + s.x, p.y + s.y);
	const bool loud = alert(colour);
	dl->AddRectFilled(p, q, col(loud ? with_alpha(colour, kPillFill) : with_alpha(g_pal.text, kCalmFill)), r);
	dl->AddRect(p, q, col(loud ? with_alpha(colour, kPillEdge) : with_alpha(g_pal.text, kCalmEdge)), r, 0, hairline());
	status_dot(dl, ImVec2(p.x + 8.0f * k + dot + (g_icons ? 1.0f * k : 0.0f), p.y + r), colour);
	const Span span{ t, pill_text_colour(colour) };
	Layout l;
	lay_out(&span, 1, max_w > 0.0f ? pill_wrap(t, max_w) : 1.0e9f, l);
	draw_layout(dl, l, ImVec2(p.x + pill_lead(), p.y + 3.0f * k), &span, ImGui::GetTextLineHeight());
}

float card_rounding()
{
	return 7.0f * unit();
}

void begin_card(const char *id)
{
	ImGuiStorage *st = ImGui::GetStateStorage();
	ImGui::PushID(id);
	const ImGuiID hid = ImGui::GetID("##card_height");
	const ImGuiID sid = ImGui::GetID("##card_stack");
	ImGui::PopID();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	const float h = st->GetFloat(hid, 0.0f);
	if (h > 0.0f) {
		ImDrawList *dl = ImGui::GetWindowDrawList();
		const float r = card_rounding();
		const float t = hairline();
		const ImVec2 a(p.x, p.y - t), b(p.x + w, p.y + h);
		dl->AddRectFilled(a, b, col(g_pal.surface), r, ImDrawFlags_RoundCornersBottom);
		dl->AddRect(a, b, col(g_pal.border), r, ImDrawFlags_RoundCornersBottom, t);
	}
	if (g_card_depth < kMaxCards)
		g_cards[g_card_depth] = Card{ p, p.x + w, card_pad(), hid, sid, st->GetInt(sid, 0) != 0, false };
	++g_card_depth;

	const float pad = card_pad();
	const ImGuiStyle &style = ImGui::GetStyle();
	ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, pad - style.ItemSpacing.y * 2.0f)));
	ImGui::Indent(pad);
	ImGui::PushTextWrapPos(p.x + w - pad - ImGui::GetWindowPos().x + ImGui::GetScrollX());
}

ImVec4 banner_left()
{
	return mix(g_pal.header, g_pal.text, g_dark ? 0.02f : 0.0f);
}

ImVec4 g_banner_accent[2];

ImVec4 banner_second()
{
	return g_banner_accent[0];
}

ImVec4 opaque(ImVec4 c);
ImVec4 reach(const ImVec4 &c, const ImVec4 *bgs, int n, float min);

float mark_letter_width(char c)
{
	switch (c) {
	case 'A': return 0.80f;
	case 'C': return 0.84f;
	case 'D': return 0.72f;
	case 'E': return 0.56f;
	case 'F': return 0.52f;
	case 'G': return 0.90f;
	case 'I': return 0.0f;
	case 'L': return 0.50f;
	case 'M': return 0.86f;
	case 'N': return 0.70f;
	case 'O': return 1.00f;
	case 'P': return 0.58f;
	case 'R': return 0.62f;
	case 'S': return 0.60f;
	case 'T': return 0.62f;
	case 'U': return 0.68f;
	case 'X': return 0.72f;
	case '-': return 0.36f;
	case '(':
	case ')': return 0.30f;
	case ' ': return 0.30f;
	default: return -1.0f;
	}
}

float letter_w(char c, float h, float t)
{
	return c == 'I' ? t : mark_letter_width(c) * h;
}

constexpr float kMarkTracking = 0.24f;

bool mark_can_draw(const char *s)
{
	for (; *s != '\0'; ++s)
		if (mark_letter_width(static_cast<char>(std::toupper(static_cast<unsigned char>(*s)))) < 0.0f)
			return false;
	return true;
}

float mark_width(const char *s, float h)
{
	float w = 0.0f;
	for (const char *c = s; *c != '\0'; ++c)
		w += mark_letter_width(*c) * h + (c[1] != '\0' ? kMarkTracking * h : 0.0f);
	return w;
}

void arc_points(std::vector<ImVec2> &out, const ImVec2 &c, float rx, float ry, float a0, float a1, int n)
{
	for (int i = 0; i <= n; ++i) {
		const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(n);
		out.push_back(ImVec2(c.x + rx * std::cos(a), c.y + ry * std::sin(a)));
	}
}

void mark_letter(ImDrawList *dl, char ch, const ImVec2 &o, float h, float t, ImU32 c)
{
	const float w = letter_w(ch, h, t);
	const float s = std::round(t * 1.3f);
	const float pi = 3.14159265f;
	const auto bar = [&](float x0, float y0, float x1, float y1) {
		dl->AddRectFilled(ImVec2(o.x + x0, o.y + y0), ImVec2(o.x + x1, o.y + y1), c);
	};
	const auto slant = [&](float tx, float bx) {
		const ImVec2 p[] = { ImVec2(o.x + tx, o.y), ImVec2(o.x + tx + s, o.y), ImVec2(o.x + bx + s, o.y + h),
			ImVec2(o.x + bx, o.y + h) };
		dl->AddConvexPolyFilled(p, 4, c);
	};
	std::vector<ImVec2> pts;
	const auto at = [&](float x, float y) { pts.push_back(ImVec2(o.x + x, o.y + y)); };
	const auto stroke = [&]() {
		dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), c, ImDrawFlags_None, t);
		pts.clear();
	};
	const auto bowl = [&](float bottom) {
		const float r = (bottom - t) * 0.5f;
		const float bx = w - t * 0.5f - r;
		at(t * 0.5f, t * 0.5f);
		arc_points(pts, ImVec2(o.x + bx, o.y + t * 0.5f + r), r, r, -0.5f * pi, 0.5f * pi, 16);
		at(t * 0.5f, bottom - t * 0.5f);
		stroke();
	};
	switch (ch) {
	case 'C':
	case 'G': {
		const ImVec2 centre(o.x + w * 0.5f, o.y + h * 0.5f);
		const float rx = (w - t) * 0.5f, ry = (h - t) * 0.5f;
		if (ch == 'C') {
			arc_points(pts, centre, rx, ry, -0.24f * pi, -1.76f * pi, 30);
		} else {
			arc_points(pts, centre, rx, ry, -0.24f * pi, -2.0f * pi, 32);
			at(w * 0.54f, h * 0.5f);
		}
		stroke();
		break;
	}
	case 'D': {
		bar(0.0f, 0.0f, t, h);
		const float r = (h - t) * 0.5f;
		at(t * 0.5f, t * 0.5f);
		arc_points(pts, ImVec2(o.x + w - t * 0.5f - r, o.y + h * 0.5f), r, r, -0.5f * pi, 0.5f * pi, 20);
		at(t * 0.5f, h - t * 0.5f);
		stroke();
		break;
	}
	case 'F': {
		bar(0.0f, 0.0f, t, h);
		bar(0.0f, 0.0f, w, t);
		const float y = std::round((h - t) * 0.5f);
		bar(0.0f, y, std::round(w * 0.86f), y + t);
		break;
	}
	case 'I':
		bar(0.0f, 0.0f, t, h);
		break;
	case 'L':
		bar(0.0f, 0.0f, t, h);
		bar(0.0f, h - t, w, h);
		break;
	case 'M': {
		bar(0.0f, 0.0f, t, h);
		bar(w - t, 0.0f, w, h);
		at(t * 0.5f, t * 0.5f);
		at(w * 0.5f, h * 0.70f);
		at(w - t * 0.5f, t * 0.5f);
		stroke();
		break;
	}
	case 'P':
		bar(0.0f, 0.0f, t, h);
		bowl(std::round(h * 0.60f));
		break;
	case 'T': {
		bar(0.0f, 0.0f, w, t);
		const float x = std::round((w - t) * 0.5f);
		bar(x, 0.0f, x + t, h);
		break;
	}
	case 'U': {
		const float r = (w - t) * 0.5f;
		at(t * 0.5f, 0.0f);
		arc_points(pts, ImVec2(o.x + w * 0.5f, o.y + h - t * 0.5f - r), r, r, pi, 0.0f, 20);
		at(w - t * 0.5f, 0.0f);
		stroke();
		break;
	}
	case 'X':
		slant(0.0f, w - s);
		slant(w - s, 0.0f);
		break;
	case '-': {
		const float y = std::round((h - t) * 0.58f);
		bar(0.0f, y, w, y + t);
		break;
	}
	case '(':
	case ')': {
		const float ry = h * 0.62f;
		const float a = std::asin(std::min(0.98f, (h - t) * 0.5f / ry));
		const float rx = (w - t) / (1.0f - std::cos(a));
		const float cy = o.y + h * 0.5f;
		if (ch == '(')
			arc_points(pts, ImVec2(o.x + t * 0.5f + rx, cy), rx, ry, pi + a, pi - a, 16);
		else
			arc_points(pts, ImVec2(o.x + w - t * 0.5f - rx, cy), rx, ry, -a, a, 16);
		stroke();
		break;
	}
	case 'A': {
		const float apex = std::round((w - s) * 0.5f);
		slant(apex, 0.0f);
		slant(apex, w - s);
		const float y = std::round(h * 0.64f);
		const float f = (y + t * 0.5f) / h;
		bar(apex + s + (s - apex - s) * f - 0.5f, y, apex + (w - s - apex) * f + 0.5f, y + t);
		break;
	}
	case 'E': {
		bar(0.0f, 0.0f, t, h);
		bar(0.0f, 0.0f, w, t);
		const float y = std::round((h - t) * 0.5f);
		bar(0.0f, y, std::round(w * 0.9f), y + t);
		bar(0.0f, h - t, w, h);
		break;
	}
	case 'O':
		dl->AddCircle(ImVec2(o.x + w * 0.5f, o.y + h * 0.5f), (h - t) * 0.5f, c, 0, t);
		break;
	case 'N':
		bar(0.0f, 0.0f, t, h);
		bar(w - t, 0.0f, w, h);
		slant(0.0f, w - s);
		break;
	case 'S': {
		const float rx = (w - t) * 0.5f, ry = (h - t) * 0.25f;
		const float cx = o.x + w * 0.5f;
		arc_points(pts, ImVec2(cx, o.y + t * 0.5f + ry), rx, ry, -0.16f * pi, -1.5f * pi, 18);
		pts.pop_back();
		arc_points(pts, ImVec2(cx, o.y + h - t * 0.5f - ry), rx, ry, -0.5f * pi, 0.84f * pi, 18);
		stroke();
		break;
	}
	case 'R': {
		bar(0.0f, 0.0f, t, h);
		const float bottom = std::round(h * 0.56f);
		bowl(bottom);
		const ImVec2 leg[] = { ImVec2(o.x + std::round(w * 0.36f), o.y + bottom - t),
			ImVec2(o.x + std::round(w * 0.36f) + s, o.y + bottom - t), ImVec2(o.x + w, o.y + h),
			ImVec2(o.x + w - s, o.y + h) };
		dl->AddConvexPolyFilled(leg, 4, c);
		break;
	}
	default:
		break;
	}
}

float caps_advance(char c, const char *next, float h, float t, float track)
{
	return std::round(letter_w(c, h, t) + (*next != '\0' ? track * h : 0.0f));
}

float caps_width(const char *s, float h, float t, float track)
{
	float w = 0.0f;
	for (const char *c = s; *c != '\0'; ++c)
		w += caps_advance(static_cast<char>(std::toupper(static_cast<unsigned char>(*c))), c + 1, h, t, track);
	return w;
}

void caps_text(ImDrawList *dl, float x, float y, const char *s, float h, float t, float track, ImU32 c)
{
	for (const char *ch = s; *ch != '\0'; ++ch) {
		const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(*ch)));
		mark_letter(dl, u, ImVec2(x, y), h, t, c);
		x += caps_advance(u, ch + 1, h, t, track);
	}
}

float title_stroke(float cap)
{
	return std::max(1.0f, std::round(cap * 0.165f));
}

constexpr float kTitleTracking = 0.15f;

ImVec4 switch_on(bool hovered, bool end = false)
{
	const ImVec4 &c = end ? g_pal.accent_end : g_pal.accent;
	return hovered ? mix(c, g_dark ? g_pal.knob : ImVec4(0.0f, 0.0f, 0.0f, 1.0f), g_dark ? 0.10f : 0.08f) : c;
}

ImVec4 knob_on()
{
	return g_pal.knob;
}

ImVec4 g_switch_edge[2];

ImVec4 g_chip_edge;
ImVec4 g_chip_text;

ImVec4 g_subsection;

ImVec4 switch_edge(bool hovered)
{
	return g_switch_edge[hovered ? 1 : 0];
}

ImVec4 held_header()
{
	return mix(g_pal.header, g_pal.text, 0.05f);
}

ImVec4 slider_groove()
{
	return mix(g_pal.surface_hi, g_pal.text, g_dark ? 0.16f : 0.14f);
}

ImVec4 progress_fill()
{
	return mix(g_pal.surface_hi, g_pal.text, g_dark ? 0.16f : 0.12f);
}

constexpr float kHaloOuter = 0.07f;
constexpr float kHaloInner = 0.14f;

float halo_share()
{
	return 1.0f - (1.0f - kHaloOuter) * (1.0f - kHaloInner);
}

ImVec4 tile_unlit()
{
	return mix(g_pal.header, g_pal.text, g_dark ? 0.09f : 0.075f);
}

ImVec4 opaque(ImVec4 c)
{
	c.w = 1.0f;
	return c;
}

ImVec4 over(const ImVec4 &bg, const ImVec4 &fg, float a)
{
	return mix(opaque(bg), opaque(fg), a);
}

ImVec4 reach(const ImVec4 &c, const ImVec4 *bgs, int n, float min)
{
	const float top = std::max({ c.x, c.y, c.z, 0.001f });
	const ImVec4 bright(c.x / top, c.y / top, c.z / top, 1.0f);
	const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f), black(0.0f, 0.0f, 0.0f, 1.0f);
	for (int i = 0; i <= 50; ++i) {
		const float t = static_cast<float>(i) / 25.0f;
		const ImVec4 v = !g_dark ? mix(opaque(c), black, t * 0.5f)
			: t <= 1.0f ? mix(opaque(c), bright, t) : mix(bright, white, t - 1.0f);
		bool ok = true;
		for (int j = 0; j < n && ok; ++j)
			ok = contrast(v, bgs[j]) >= min;
		if (ok)
			return v;
	}
	return g_dark ? white : black;
}

ImVec4 tile_lit(const ImVec4 &brand, const ImVec4 &header)
{
	return over(header, brand, g_dark ? 0.20f : 0.14f);
}

ImVec4 tile_glyph(const ImVec4 &brand)
{
	const ImVec4 grounds[] = { tile_lit(brand, g_pal.header), tile_lit(brand, mix(g_pal.header, g_pal.text, 0.025f)),
		tile_lit(brand, held_header()) };
	return reach(brand, grounds, 3, 3.02f);
}

ImVec4 g_rim[5];

int rim_slot(const ImVec4 *c)
{
	return c == &g_pal.dlss || c == &g_pal.vsr ? 0 : c == &g_pal.fsr ? 1 : c == &g_pal.xess ? 2
		: c == &g_pal.accent ? 3 : 4;
}

const ImVec4 &rim(const ImVec4 *c)
{
	return g_rim[rim_slot(c)];
}

ImVec4 g_chosen_ink;

constexpr float kUnlitTint = 0.20f;

ImVec4 g_tint_faded[3];

ImVec4 raised_fill(const ImVec4 *tint)
{
	return tint != nullptr && !g_dark ? mix(opaque(g_pal.raised), opaque(*tint), kUnlitTint) : g_pal.raised;
}

void accent_chip(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, float rounding)
{
	dl->AddRectFilled(a, b, col(g_pal.accent), rounding);
	dl->AddRect(a, b, col(g_rim[3]), rounding, 0, hairline());
}

void lift(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, float rounding)
{
	if (g_dark)
		return;
	const float k = unit();
	const float d = std::max(1.0f, std::floor(1.5f * k));
	dl->AddRectFilled(ImVec2(a.x + 0.5f * k, a.y + d), ImVec2(b.x - 0.5f * k, b.y + d),
		col(ImVec4(0.10f, 0.11f, 0.22f, 0.085f)), rounding);
}

}

const Palette &palette()
{
	return g_pal;
}

const ImVec4 *brand_colour(Brand b)
{
	switch (b) {
	case Brand::Nvidia: return &g_pal.dlss;
	case Brand::Amd: return &g_pal.fsr;
	case Brand::Intel: return &g_pal.xess;
	case Brand::Neutral:
	case Brand::None: break;
	}
	return nullptr;
}

ImVec4 mix(const ImVec4 &a, const ImVec4 &b, float t)
{
	return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

namespace {

float linear(float c)
{
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float luminance(const ImVec4 &c)
{
	return 0.2126f * linear(c.x) + 0.7152f * linear(c.y) + 0.0722f * linear(c.z);
}

}

float contrast(const ImVec4 &a, const ImVec4 &b)
{
	const float la = luminance(a), lb = luminance(b);
	return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

ImVec4 readable_on(const ImVec4 &bg)
{
	const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
	const ImVec4 ink(0.07f, 0.08f, 0.09f, 1.0f);
	return contrast(white, bg) >= contrast(ink, bg) ? white : ink;
}

ImVec4 with_alpha(ImVec4 c, float a)
{
	c.w *= a;
	return c;
}

Theme::Theme()
{
	const ImGuiStyle &style = ImGui::GetStyle();
	ImVec4 bg = style.Colors[ImGuiCol_WindowBg];
	bg.w = 1.0f;
	const ImVec4 text = style.Colors[ImGuiCol_Text];
	const ImVec4 ink(text.x, text.y, text.z, 1.0f);
	const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
	const bool dark = 0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z < 0.5f;
	g_dark = dark;

	Palette &p = g_pal;
	p.text = text;
	p.muted = mix(ink, bg, dark ? 0.22f : 0.28f);
	p.label = mix(ink, bg, dark ? 0.16f : 0.22f);
	if (dark) {
		p.surface = mix(bg, ink, 0.045f);
		p.header = mix(bg, ink, 0.085f);
		p.surface_hi = mix(bg, ink, 0.10f);
		p.border = mix(bg, ink, 0.165f);
	} else {
		p.surface = white;
		p.header = mix(bg, white, 0.40f);
		p.surface_hi = mix(bg, ink, 0.035f);
		p.border = mix(bg, ink, 0.13f);
	}
	p.tray = dark ? mix(p.surface, bg, 0.55f) : mix(p.surface, p.surface_hi, 0.30f);
	p.band = dark ? bg : mix(p.surface, p.surface_hi, 0.50f);
	p.accent = ImVec4(0.487f, 0.344f, 0.956f, 1.0f);
	p.accent_end = ImVec4(0.589f, 0.302f, 0.918f, 1.0f);
	p.knob = white;
	p.raised = dark ? mix(p.surface_hi, ink, 0.24f) : white;
	p.loading = dark ? ImVec4(0.42f, 0.53f, 1.00f, 1.0f) : ImVec4(0.27f, 0.37f, 0.92f, 1.0f);
	p.ok = dark ? ImVec4(0.24f, 0.84f, 0.63f, 1.0f) : ImVec4(0.02f, 0.56f, 0.42f, 1.0f);
	p.warn = dark ? ImVec4(1.00f, 0.75f, 0.33f, 1.0f) : ImVec4(0.561f, 0.322f, 0.000f, 1.0f);
	p.fail = dark ? ImVec4(1.00f, 0.46f, 0.55f, 1.0f) : ImVec4(0.741f, 0.110f, 0.290f, 1.0f);
	p.off = p.muted;
	{
		const ImVec4 grounds[] = { over(p.surface, ink, kCalmFill), over(bg, ink, kCalmFill),
			over(p.band, ink, kCalmFill), over(banner_left(), ink, kCalmFill), over(p.tray, ink, 0.03f),
			opaque(p.surface) };
		p.ok = reach(p.ok, grounds, 6, 3.02f);
	}
	{
		const ImVec4 floors[] = { opaque(p.surface), opaque(p.tray), opaque(p.band),
			over(p.surface, p.surface_hi, kDimAlpha) };
		p.faded = p.muted;
		for (int i = 0; i <= 100; ++i) {
			const ImVec4 v = mix(bg, ink, static_cast<float>(i) / 100.0f);
			bool ok = true;
			for (const ImVec4 &f : floors)
				ok = ok && contrast(v, f) >= 4.6f;
			if (ok) {
				p.faded = v;
				break;
			}
		}
	}
	p.dlss = ImVec4(0.463f, 0.725f, 0.000f, 1.0f);
	p.vsr = p.dlss;
	p.fsr = ImVec4(0.886f, 0.137f, 0.165f, 1.0f);
	p.xess = ImVec4(0.000f, 0.443f, 0.773f, 1.0f);
	{
		const ImVec4 *brands[] = { &p.dlss, &p.fsr, &p.xess };
		const ImVec4 faded_floor = over(p.surface, p.surface_hi, kDimAlpha);
		for (int i = 0; i < 3; ++i) {
			const ImVec4 grounds[] = { over(p.surface_hi, *brands[i], halo_share()), opaque(p.surface_hi),
				opaque(p.header), opaque(held_header()), opaque(p.surface), opaque(banner_left()) };
			g_rim[i] = reach(*brands[i], grounds, 6, 3.02f);
			const ImVec4 tinted = over(faded_floor, *brands[i], kUnlitTint);
			g_tint_faded[i] = dark ? p.faded : reach(p.faded, &tinted, 1, 4.52f);
		}
		const ImVec4 control = opaque(p.surface_hi);
		const ImVec4 own = opaque(p.accent);
		const ImVec4 to_control = reach(p.accent, &control, 1, 3.02f);
		const ImVec4 to_fill = reach(p.accent, &own, 1, contrast(g_rim[0], opaque(p.dlss)));
		g_rim[3] = contrast(to_fill, own) > contrast(to_control, own) ? to_fill : to_control;
		g_rim[4] = reach(mix(p.surface_hi, ink, 0.40f), &control, 1, 3.02f);
		g_chosen_ink = readable_on(p.accent);
		const ImVec4 rail[] = { control, slider_groove(), over(p.surface_hi, p.text, 0.05f) };
		p.accent_mark = reach(p.accent, rail, 3, 3.02f);
		p.accent_mark_end = reach(p.accent_end, rail, 3, 3.02f);
		const ImVec4 banner_grounds[] = { opaque(banner_left()), opaque(p.surface) };
		g_banner_accent[0] = reach(p.accent_mark, banner_grounds, 2, 4.52f);
		g_banner_accent[1] = reach(p.accent_mark_end, banner_grounds, 2, 4.52f);
		const ImVec4 grounds[] = { opaque(p.surface), opaque(p.tray), opaque(p.band) };
		g_subsection = reach(mix(p.label, g_banner_accent[0], dark ? 0.50f : 0.55f), grounds, 3, 4.52f);
		g_switch_edge[0] = reach(mix(p.surface, ink, 0.40f), grounds, 3, 3.02f);
		g_switch_edge[1] = mix(g_switch_edge[0], ink, 0.25f);
		const ImVec4 chip_grounds[] = { opaque(p.header), opaque(mix(p.header, p.text, 0.025f)),
			opaque(held_header()), opaque(p.surface) };
		g_chip_edge = reach(mix(p.surface, ink, 0.40f), chip_grounds, 4, 3.02f);
		g_chip_text = reach(p.label, chip_grounds, 4, 4.52f);
	}

	g_icons = ImGui::GetFont() != nullptr && ImGui::GetFont()->IsGlyphInFont(0xF219);
	g_card_depth = 0;
	g_caption_fit_id = ImGui::GetID("##aeon_caption_fit");
	g_caption_fit = ImGui::GetStateStorage()->GetFloat(g_caption_fit_id, 0.0f);
	g_caption_next = 0.0f;
	g_value_fit_id = ImGui::GetID("##aeon_value_fit");
	g_value_fit = ImGui::GetStateStorage()->GetFloat(g_value_fit_id, 0.0f);
	g_value_next = 0.0f;
	g_title_fit_id = ImGui::GetID("##aeon_title_fit");
	g_title_fit = ImGui::GetStateStorage()->GetFloat(g_title_fit_id, 0.0f);
	g_title_next = 1.0e9f;
	g_title_caps_id = ImGui::GetID("##aeon_title_caps");
	g_title_caps = ImGui::GetStateStorage()->GetInt(g_title_caps_id, 1) != 0;
	g_title_caps_next = true;

	const auto colour = [this](ImGuiCol idx, const ImVec4 &c) {
		ImGui::PushStyleColor(idx, c);
		++colours_;
	};
	colour(ImGuiCol_TextDisabled, p.muted);
	colour(ImGuiCol_FrameBg, p.surface_hi);
	colour(ImGuiCol_FrameBgHovered, mix(p.surface_hi, ink, 0.06f));
	colour(ImGuiCol_FrameBgActive, mix(p.surface_hi, ink, 0.10f));
	colour(ImGuiCol_Button, p.surface_hi);
	colour(ImGuiCol_ButtonHovered, mix(p.surface_hi, ink, 0.08f));
	colour(ImGuiCol_ButtonActive, mix(p.surface_hi, ink, 0.14f));
	colour(ImGuiCol_CheckMark, p.accent);
	colour(ImGuiCol_SliderGrab, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	colour(ImGuiCol_SliderGrabActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	colour(ImGuiCol_Header, p.surface_hi);
	colour(ImGuiCol_HeaderHovered, mix(p.surface_hi, ink, 0.08f));
	colour(ImGuiCol_HeaderActive, mix(p.surface_hi, ink, 0.14f));
	colour(ImGuiCol_Separator, p.border);
	colour(ImGuiCol_PlotLines, p.label);
	colour(ImGuiCol_PlotHistogram, progress_fill());
	colour(ImGuiCol_TableRowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	colour(ImGuiCol_TableRowBgAlt, with_alpha(text, 0.03f));
	colour(ImGuiCol_TableBorderLight, p.border);

	const float k = unit();
	const auto var = [this](ImGuiStyleVar idx, float v) {
		ImGui::PushStyleVar(idx, v);
		++vars_;
	};
	const auto var2 = [this](ImGuiStyleVar idx, const ImVec2 &v) {
		ImGui::PushStyleVar(idx, v);
		++vars_;
	};
	var(ImGuiStyleVar_FrameRounding, 5.0f * k);
	var(ImGuiStyleVar_GrabRounding, 2.0f * k);
	var(ImGuiStyleVar_PopupRounding, 6.0f * k);
	var2(ImGuiStyleVar_FramePadding, ImVec2(8.0f * k, 4.0f * k));
	var2(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * k, 6.0f * k));
	var2(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6.0f * k, 4.0f * k));
	var2(ImGuiStyleVar_CellPadding, ImVec2(5.0f * k, 3.0f * k));
}

Theme::~Theme()
{
	while (g_card_depth > 0)
		section_end();
	ImGui::GetStateStorage()->SetFloat(g_caption_fit_id, g_caption_next);
	ImGui::GetStateStorage()->SetFloat(g_value_fit_id, g_value_next);
	ImGui::GetStateStorage()->SetFloat(g_title_fit_id, g_title_next < 1.0e9f ? g_title_next : 0.0f);
	ImGui::GetStateStorage()->SetInt(g_title_caps_id, g_title_caps_next ? 1 : 0);
	ImGui::PopStyleVar(vars_);
	ImGui::PopStyleColor(colours_);
}

Dim::Dim(bool on) : on_(on)
{
	if (!on_)
		return;
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * kDimAlpha);
	ImGui::PushStyleColor(ImGuiCol_Text, dim_text());
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, dim_text());
	++g_dim;
}

Dim::~Dim()
{
	if (!on_)
		return;
	--g_dim;
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(1);
}

void tooltip_alpha_begin()
{
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
	g_tip_colours = 0;
	if (g_dim > 0) {
		ImGui::PushStyleColor(ImGuiCol_Text, g_pal.text);
		ImGui::PushStyleColor(ImGuiCol_TextDisabled, g_pal.muted);
		g_tip_colours = 2;
	}
}

void tooltip_alpha_end()
{
	ImGui::PopStyleColor(g_tip_colours);
	g_tip_colours = 0;
	ImGui::PopStyleVar(1);
}

const char *glyph(const char *icon)
{
	return g_icons && icon != nullptr ? icon : "";
}

void draw_logo(ImDrawList *dl, const ImVec2 &p, float size)
{
	const float s = size / 48.0f;
	const auto at = [&](float x, float y) { return ImVec2(p.x + x * s, p.y + y * s); };
	const ImVec4 tile_top(0x27 / 255.0f, 0x27 / 255.0f, 0x2A / 255.0f, 1.0f);
	const ImVec4 tile_bottom(0x09 / 255.0f, 0x09 / 255.0f, 0x0B / 255.0f, 1.0f);
	const ImVec4 silver_light(1.0f, 1.0f, 1.0f, 1.0f);
	const ImVec4 silver_dark(0xA1 / 255.0f, 0xA1 / 255.0f, 0xAA / 255.0f, 1.0f);
	const ImU32 white = IM_COL32(255, 255, 255, 255);

	int first = dl->VtxBuffer.Size;
	dl->AddRectFilled(at(0.0f, 0.0f), at(48.0f, 48.0f), white, 9.0f * s);
	shade(dl, first, at(24.0f, 0.0f), at(24.0f, 48.0f), tile_top, tile_bottom);

	first = dl->VtxBuffer.Size;
	const float arc_w = std::max(1.0f, 2.2f * s);
	const ImVec2 centre = at(24.0f, 24.4122f);
	dl->PathArcTo(centre, 16.0f * s, 2.5749f, 6.8499f, 48);
	dl->PathStroke(white, ImDrawFlags_None, arc_w);
	dl->AddCircleFilled(at(10.5f, 33.0f), arc_w * 0.5f, white, 12);
	dl->AddCircleFilled(at(37.5f, 33.0f), arc_w * 0.5f, white, 12);
	shade(dl, first, at(8.0f, 8.4f), at(40.0f, 33.0f), silver_light, silver_dark);

	first = dl->VtxBuffer.Size;
	const ImVec2 a[] = { at(24.0f, 4.0f), at(38.0f, 42.0f), at(32.0f, 39.0f), at(24.0f, 17.0f), at(16.0f, 39.0f),
		at(10.0f, 42.0f) };
	dl->AddConcavePolyFilled(a, 6, white);
	shade(dl, first, at(10.0f, 4.0f), at(38.0f, 42.0f), silver_light, silver_dark);

	const float red_w = std::max(1.0f, 1.6f * s);
	const ImU32 red = IM_COL32(0xEF, 0x44, 0x44, 255);
	dl->AddLine(at(24.0f, 30.0f), at(24.0f, 41.0f), red, red_w);
	dl->AddCircleFilled(at(24.0f, 30.0f), red_w * 0.5f, red, 8);
	dl->AddCircleFilled(at(24.0f, 41.0f), red_w * 0.5f, red, 8);

	const ImVec2 spark[] = { at(31.0f, 13.0f), at(32.0f, 17.0f), at(36.0f, 18.0f), at(32.0f, 19.0f), at(31.0f, 23.0f),
		at(30.0f, 19.0f), at(26.0f, 18.0f), at(30.0f, 17.0f) };
	dl->AddConcavePolyFilled(spark, 8, white);
}

void banner(const char *title, const char *subtitle)
{
	const float k = unit();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	const float pad = 12.0f * k;
	const float r = card_rounding();

	char word[32]{};
	const char *rest = title;
	size_t n = 0;
	while (*rest != '\0' && *rest != ' ' && n + 1 < sizeof(word))
		word[n++] = *rest++;
	const char *second = *rest != '\0' ? rest + 1 : rest;

	const bool drawn = mark_can_draw(title);
	const float cap = std::round(17.0f * k);
	const float stroke = std::max(1.0f, std::round(2.1f * k));
	const float tracking = 3.0f * k;
	float title_h = cap, title_w = 0.0f, space = cap * 0.62f;
	if (drawn) {
		title_w = mark_width(word, cap) + (*second != '\0' ? space + mark_width(second, cap) : 0.0f);
	} else {
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
		title_h = ImGui::GetFontSize();
		space = ImGui::CalcTextSize(" ").x + tracking;
		title_w = tracked_width(word, tracking) + (*second != '\0' ? space + tracked_width(second, tracking) : 0.0f);
		ImGui::PopFont();
	}
	const ImVec2 sub = ImGui::CalcTextSize(subtitle);
	const float ring = 10.4f * k;
	const float text_x = pad + ring * 2.0f + 12.0f * k;
	const bool stacked = text_x + title_w + 16.0f * k + sub.x > w - pad;
	const float gap = 7.0f * k;
	const float h = stacked ? title_h + gap + sub.y + 22.0f * k : std::max(title_h, ring * 2.0f) + 22.0f * k;
	const ImVec2 q(p.x + w, p.y + h);

	lift(dl, p, q, r);
	gradient_rect(dl, p, q, banner_left(), g_pal.surface, r, ImDrawFlags_RoundCornersAll);
	dl->AddRect(p, q, col(g_pal.border), r, ImDrawFlags_RoundCornersAll, hairline());

	const float logo = std::round(ring * 2.0f + 4.0f * k);
	draw_logo(dl, ImVec2(std::round(p.x + pad - 2.0f * k), std::round(p.y + (h - logo) * 0.5f)), logo);

	const float block = stacked ? title_h + gap + sub.y : title_h;
	const float ty = std::round(p.y + (h - block) * 0.5f);
	const float tx = std::round(p.x + text_x);
	if (drawn) {
		float x = tx;
		const auto letters = [&](const char *s, const ImVec4 &colour) {
			for (const char *ch = s; *ch != '\0'; ++ch) {
				mark_letter(dl, *ch, ImVec2(x, ty), cap, stroke, col(colour));
				x += std::round(mark_letter_width(*ch) * cap + kMarkTracking * cap);
			}
		};
		letters(word, g_pal.text);
		if (*second != '\0') {
			x += std::round(space - kMarkTracking * cap);
			const float x0 = x;
			const int first = dl->VtxBuffer.Size;
			letters(second, banner_second());
			shade(dl, first, ImVec2(x0, ty), ImVec2(x - kMarkTracking * cap, ty), g_banner_accent[0],
				g_banner_accent[1]);
		}
	} else {
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
		const float x = tracked_text(dl, tx, ty, g_pal.text, word, tracking);
		if (*second != '\0') {
			const float x0 = x + space - tracking;
			const int first = dl->VtxBuffer.Size;
			const float x1 = tracked_text(dl, x0, ty, banner_second(), second, tracking);
			shade(dl, first, ImVec2(x0, ty), ImVec2(x1, ty), g_banner_accent[0], g_banner_accent[1]);
		}
		ImGui::PopFont();
	}

	if (stacked)
		dl->AddText(ImVec2(tx, ty + title_h + gap), col(g_pal.muted), subtitle);
	else
		dl->AddText(ImVec2(q.x - pad - sub.x, p.y + (h - sub.y) * 0.5f), col(g_pal.muted), subtitle);

	ImGui::Dummy(ImVec2(w, h));
	block_gap(q.y);
}

namespace {

ImVec2 chip_size(const char *text)
{
	const ImVec2 s = ImGui::CalcTextSize(text);
	return ImVec2(std::floor(s.x + 14.0f * unit()), std::floor(pill_line()));
}

void chip_at(ImDrawList *dl, const ImVec2 &p, const char *text, Brand b, bool lit)
{
	const float k = unit();
	const ImVec2 s = chip_size(text);
	const ImVec2 q(p.x + s.x, p.y + s.y);
	const float r = 4.0f * k;
	const float t = hairline();
	const ImVec4 *c = brand_colour(b);
	ImVec4 text_colour = g_chip_text;
	if (lit && c != nullptr) {
		dl->AddRectFilled(p, q, col(*c), r);
		dl->AddRectFilled(ImVec2(p.x + r, p.y + t), ImVec2(q.x - r, p.y + t * 2.0f),
			col(ImVec4(1.0f, 1.0f, 1.0f, 0.22f)));
		dl->AddRect(p, q, col(rim(c)), r, 0, t);
		text_colour = readable_on(*c);
	} else {
		dl->AddRect(p, q, col(g_chip_edge), r, 0, t);
	}
	const ImVec2 ts = ImGui::CalcTextSize(text);
	dl->AddText(ImVec2(std::floor(p.x + (s.x - ts.x) * 0.5f), std::floor(p.y + (s.y - ts.y) * 0.5f)),
		col(text_colour), text);
}

}

namespace {

bool section_icon(ImDrawList *dl, const ImVec2 &c, float s, const char *glyph, ImU32 colour)
{
	const float t = std::max(1.0f, s * 0.09f);
	const float h = s * 0.5f;
	if (std::strcmp(glyph, icon::kTaa) == 0) {
		const float w = s * 0.56f, d = s * 0.2f;
		for (int i = 2; i >= 0; --i) {
			const ImVec2 a(c.x - h + d * static_cast<float>(i), c.y - h + d * static_cast<float>(2 - i));
			if (i == 0)
				dl->AddRectFilled(a, ImVec2(a.x + w, a.y + w), colour, t);
			else
				dl->AddRect(a, ImVec2(a.x + w, a.y + w), colour, t, 0, t);
		}
		return true;
	}
	if (std::strcmp(glyph, icon::kNeural) == 0) {
		const ImVec2 in[2] = { ImVec2(c.x - h * 0.8f, c.y - h * 0.5f), ImVec2(c.x - h * 0.8f, c.y + h * 0.5f) };
		const ImVec2 mid[3] = { ImVec2(c.x, c.y - h * 0.8f), ImVec2(c.x, c.y), ImVec2(c.x, c.y + h * 0.8f) };
		const ImVec2 out(c.x + h * 0.8f, c.y);
		for (const ImVec2 &a : in)
			for (const ImVec2 &b : mid)
				dl->AddLine(a, b, colour, t * 0.7f);
		for (const ImVec2 &b : mid)
			dl->AddLine(b, out, colour, t * 0.7f);
		const float r = s * 0.11f;
		for (const ImVec2 &a : in)
			dl->AddCircleFilled(a, r, colour, 12);
		for (const ImVec2 &b : mid)
			dl->AddCircleFilled(b, r, colour, 12);
		dl->AddCircleFilled(out, r * 1.35f, colour, 12);
		return true;
	}
	if (std::strcmp(glyph, icon::kExperiments) == 0) {
		const float nw = s * 0.14f, top = c.y - h, neck = c.y - h * 0.2f, base = c.y + h;
		const ImVec2 l0(c.x - nw, top), l1(c.x - nw, neck), l2(c.x - h * 0.9f, base);
		const ImVec2 r0(c.x + nw, top), r1(c.x + nw, neck), r2(c.x + h * 0.9f, base);
		const float lvl = c.y + h * 0.25f, f = (lvl - neck) / (base - neck);
		const ImVec2 ql(l1.x + (l2.x - l1.x) * f, lvl), qr(r1.x + (r2.x - r1.x) * f, lvl);
		dl->AddTriangleFilled(ql, l2, r2, colour);
		dl->AddTriangleFilled(ql, r2, qr, colour);
		dl->AddLine(l0, l1, colour, t);
		dl->AddLine(l1, l2, colour, t);
		dl->AddLine(l2, r2, colour, t);
		dl->AddLine(r2, r1, colour, t);
		dl->AddLine(r1, r0, colour, t);
		dl->AddLine(ImVec2(l0.x - nw * 0.8f, top), ImVec2(r0.x + nw * 0.8f, top), colour, t);
		return true;
	}
	if (std::strcmp(glyph, icon::kDiagnostics) == 0) {
		const ImVec2 pts[] = {
			ImVec2(c.x - h, c.y), ImVec2(c.x - h * 0.45f, c.y), ImVec2(c.x - h * 0.2f, c.y - h * 0.8f),
			ImVec2(c.x + h * 0.15f, c.y + h * 0.8f), ImVec2(c.x + h * 0.4f, c.y - h * 0.3f),
			ImVec2(c.x + h * 0.55f, c.y), ImVec2(c.x + h, c.y),
		};
		for (size_t i = 1; i < sizeof(pts) / sizeof(pts[0]); ++i)
			dl->AddLine(pts[i - 1], pts[i], colour, t * 1.2f);
		return true;
	}
	return false;
}

}

bool section(const char *id, const char *icon_glyph, const char *title, bool default_open,
	const ImVec4 *badge_colour, const char *badge, const char *tag, Brand tag_brand, bool lit, bool closed_only,
	const char *badge_short)
{
	const float k = unit();
	ImGuiStorage *st = ImGui::GetStateStorage();
	const ImGuiID sid = ImGui::GetID(id);
	bool open = st->GetInt(sid, default_open ? 1 : 0) != 0;

	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	const float row = ImGui::GetFrameHeight() + 8.0f * k;
	const float icon_w = g_icons && icon_glyph != nullptr ? ImGui::GetFontSize() * 1.25f + 6.0f * k : 0.0f;
	const float title_x = 12.0f * k + icon_w;
	const float tracking = title_tracking();
	const bool has_badge = badge != nullptr && badge_colour != nullptr;
	const bool has_tag = tag != nullptr && !(closed_only && open);
	const float tag_w = has_tag ? chip_size(tag).x + 8.0f * k : 0.0f;
	ImVec2 badge_s = has_badge ? pill_size(badge) : ImVec2(0.0f, 0.0f);

	const bool drawable = mark_can_draw(title);
	g_title_caps_next = g_title_caps_next && drawable;
	const bool caps = drawable && g_title_caps;
	const float want = std::round(11.0f * k);
	const float least = std::round(8.0f * k);
	const auto title_width = [&](float cap) {
		return caps ? caps_width(title, cap, title_stroke(cap), kTitleTracking) : tracked_width(title, tracking);
	};
	float cap = g_title_fit > 0.0f ? std::min(want, g_title_fit) : want;
	float title_w = title_width(cap);

	const char *mark = !has_badge ? nullptr
		: same(*badge_colour, g_pal.fail) ? glyph(icon::kFail)
		: same(*badge_colour, g_pal.warn) ? glyph(icon::kWarn) : "";
	const auto overflows = [&](float badge_w) {
		return title_x + title_w + 8.0f * k + 26.0f * k + badge_w + 8.0f * k + tag_w > w;
	};
	const char *badge_text = badge;
	bool compact = has_badge && overflows(badge_s.x);
	if (compact && badge_short != nullptr && !overflows(pill_size(badge_short).x)) {
		badge_text = badge_short;
		badge_s = pill_size(badge_short);
		compact = false;
	}
	const bool shortened = compact || badge_text != badge;
	if (compact)
		badge_s = ImVec2(std::floor((*mark != '\0' ? ImGui::CalcTextSize(mark).x : 6.0f * k) + 14.0f * k), pill_line());
	const float limit = w - 26.0f * k - (has_badge ? badge_s.x + 8.0f * k : 0.0f) - tag_w - 8.0f * k - title_x;
	if (caps) {
		float fit = want;
		while (fit > least && title_width(fit) > limit)
			fit -= 1.0f;
		g_title_next = std::min(g_title_next, std::max(fit, std::round(want * 0.8f)));
		if (title_w > limit) {
			cap = std::min(cap, fit);
			title_w = title_width(cap);
		}
	}

	const float h = row;
	ImGui::PushID(id);
	const bool pressed = ImGui::InvisibleButton("##section", ImVec2(w, h));
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();
	ImGui::PopID();
	if (pressed) {
		open = !open;
		st->SetInt(sid, open ? 1 : 0);
	}
	if (shortened && hovered) {
		char buf[512];
		ImGui::SetTooltip("%s", pill_caption(badge, buf));
	}

	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float r = card_rounding();
	const ImDrawFlags corners = open ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll;
	const ImVec4 base = held ? held_header() : hovered ? mix(g_pal.header, g_pal.text, 0.025f) : g_pal.header;
	const ImVec2 q(p.x + w, p.y + h);
	float block_bottom = q.y;
	if (open) {
		ImGui::PushID(id);
		block_bottom += st->GetFloat(ImGui::GetID("##card_height"), 0.0f);
		ImGui::PopID();
	}
	lift(dl, p, ImVec2(q.x, block_bottom), r);
	dl->AddRectFilled(p, q, col(base), r, corners);
	const float t = hairline();
	dl->AddRectFilled(ImVec2(p.x + r, p.y + t), ImVec2(q.x - r, p.y + t * 2.0f),
		col(with_alpha(g_dark ? g_pal.text : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), g_dark ? 0.05f : 0.8f)));
	const ImVec4 *tile = lit ? brand_colour(tag_brand) : nullptr;
	dl->AddRect(p, q, col(g_pal.border), r, corners, t);

	const float ty = p.y + (row - ImGui::GetFontSize()) * 0.5f;
	if (icon_w > 0.0f) {
		const float iw = ImGui::GetFontSize() * 1.25f;
		const ImVec2 s = ImGui::CalcTextSize(icon_glyph);
		const ImVec2 centre(p.x + 12.0f * k + iw * 0.5f, p.y + row * 0.5f);
		const float ts = std::floor(ImGui::GetFontSize() + 6.0f * k);
		const ImVec2 a(std::floor(centre.x - ts * 0.5f), std::floor(centre.y - ts * 0.5f));
		const ImVec2 b(a.x + ts, a.y + ts);
		const float tr = 4.0f * k;
		ImVec4 glyph_colour = g_pal.label;
		if (tile != nullptr) {
			glyph_colour = tile_glyph(*tile);
			dl->AddRectFilled(a, b, col(tile_lit(*tile, base)), tr);
			dl->AddRect(a, b, col(with_alpha(glyph_colour, 0.45f)), tr, 0, hairline());
		} else {
			dl->AddRectFilled(a, b, col(tile_unlit()), tr);
		}
		if (!section_icon(dl, centre, std::floor(ts * 0.62f), icon_glyph, col(glyph_colour)))
			dl->AddText(ImVec2(std::floor(centre.x - s.x * 0.5f), ty), col(glyph_colour), icon_glyph);
	}
	if (caps)
		caps_text(dl, std::round(p.x + title_x), std::round(p.y + (row - cap) * 0.5f), title, cap, title_stroke(cap),
			kTitleTracking, col(g_pal.text));
	else
		tracked_text(dl, p.x + title_x, ty, g_pal.text, title, tracking);

	const float chev_x = q.x - 14.0f * k;
	chevron(dl, ImVec2(chev_x, p.y + row * 0.5f), open, hovered ? g_pal.text : g_pal.muted);
	float end_x = chev_x - 12.0f * k;
	if (compact) {
		end_x -= badge_s.x;
		const ImVec2 a(end_x, std::floor(p.y + (row - badge_s.y) * 0.5f));
		const ImVec2 b(a.x + badge_s.x, a.y + badge_s.y);
		dl->AddRectFilled(a, b, col(with_alpha(*badge_colour, kPillFill)), badge_s.y * 0.5f);
		dl->AddRect(a, b, col(with_alpha(*badge_colour, kPillEdge)), badge_s.y * 0.5f, 0, hairline());
		const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
		if (*mark != '\0') {
			const ImVec2 s = ImGui::CalcTextSize(mark);
			dl->AddText(ImVec2(std::floor(c.x - s.x * 0.5f), std::floor(c.y - s.y * 0.5f) + icon_drop()),
				col(*badge_colour), mark);
		} else {
			status_dot(dl, c, *badge_colour);
		}
		end_x -= 8.0f * k;
	} else if (has_badge) {
		end_x -= badge_s.x;
		pill_at(dl, ImVec2(end_x, p.y + (row - badge_s.y) * 0.5f), *badge_colour, badge_text);
		end_x -= 8.0f * k;
	}
	if (has_tag) {
		const ImVec2 s = chip_size(tag);
		chip_at(dl, ImVec2(std::floor(end_x - s.x), std::floor(p.y + (row - s.y) * 0.5f)), tag, tag_brand, lit);
	}

	if (open) {
		ImGui::SetCursorScreenPos(ImVec2(p.x, q.y));
		begin_card(id);
	} else {
		block_gap(q.y);
	}
	return open;
}

void section_end()
{
	if (g_card_depth <= 0)
		return;
	--g_card_depth;
	const float pad = card_pad();
	const ImGuiStyle &style = ImGui::GetStyle();
	ImGui::PopTextWrapPos();
	ImGui::Unindent(pad);
	ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, pad - style.ItemSpacing.y * 2.0f)));
	const float bottom = ImGui::GetCursorScreenPos().y - style.ItemSpacing.y;
	if (g_card_depth < kMaxCards) {
		const Card &c = g_cards[g_card_depth];
		ImGui::GetStateStorage()->SetFloat(c.height_id, bottom - c.min.y);
		if (c.stack_id != 0)
			ImGui::GetStateStorage()->SetInt(c.stack_id, c.stack_next ? 1 : 0);
	}
	block_gap(bottom);
}

namespace {

struct Band {
	float left = 0.0f, right = 0.0f, top = 0.0f;
	ImGuiID height_id = 0, last_id = 0;
	bool open = false;
};
Band g_band;

}

void band_begin(const char *id)
{
	if (g_card_depth <= 0 || g_card_depth > kMaxCards)
		return;
	const float k = unit();
	const Card &card = g_cards[g_card_depth - 1];
	ImGui::PushID(id);
	const ImGuiID hid = ImGui::GetID("##band_height");
	const ImGuiID lid = ImGui::GetID("##band_last");
	ImGui::PopID();
	const float t = hairline();
	g_band = Band{ card.min.x + t, card.right - t, card.min.y, hid, lid, true };
	ImGuiStorage *st = ImGui::GetStateStorage();
	const float h = st->GetFloat(hid, 0.0f);
	if (h > 0.0f) {
		ImDrawList *dl = ImGui::GetWindowDrawList();
		const bool last = st->GetInt(lid, 0) != 0;
		const ImVec2 a(g_band.left, g_band.top), b(g_band.right, g_band.top + h);
		dl->AddRectFilled(a, b, col(g_pal.band), last ? card_rounding() - t : 0.0f,
			last ? ImDrawFlags_RoundCornersBottom : ImDrawFlags_None);
		dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(b.x, a.y + t),
			col(with_alpha(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), g_dark ? 0.22f : 0.045f)));
		if (!last)
			dl->AddRectFilled(ImVec2(a.x, b.y - t), b, col(g_pal.border));
	}
	ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, g_band.top + 9.0f * k));
}

void band_end(bool last)
{
	if (!g_band.open)
		return;
	g_band.open = false;
	const float k = unit();
	const float spacing = ImGui::GetStyle().ItemSpacing.y;
	const ImVec2 c = ImGui::GetCursorScreenPos();
	const float bottom = std::floor(c.y - spacing + 9.0f * k);
	ImGuiStorage *st = ImGui::GetStateStorage();
	st->SetFloat(g_band.height_id, bottom - g_band.top);
	st->SetInt(g_band.last_id, last ? 1 : 0);
	const float next = last ? bottom - std::max(0.0f, card_pad() - spacing * 2.0f) : bottom + 2.0f * k;
	ImGui::SetCursorScreenPos(ImVec2(c.x, next - spacing));
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void tray_begin(const char *id)
{
	const float k = unit();
	ImGui::PushID(id);
	const ImGuiID hid = ImGui::GetID("##tray_height");
	ImGui::PopID();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = avail_width();
	const float h = ImGui::GetStateStorage()->GetFloat(hid, 0.0f);
	if (h > 0.0f) {
		ImDrawList *dl = ImGui::GetWindowDrawList();
		const float r = 5.0f * k;
		const ImVec2 q(p.x + w, p.y + h);
		dl->AddRectFilled(p, q, col(g_pal.tray), r);
		const float t = hairline();
		dl->AddRectFilled(ImVec2(p.x + r, p.y + t), ImVec2(q.x - r, p.y + t * 2.0f),
			col(with_alpha(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), g_dark ? 0.22f : 0.045f)));
		dl->AddRect(p, q, col(g_pal.border), r, 0, t);
	}
	const float pad = 2.0f * k;
	if (g_card_depth < kMaxCards)
		g_cards[g_card_depth] = Card{ p, p.x + w, pad, hid };
	++g_card_depth;
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 4.0f * k));
	ImGui::Indent(pad);
}

void tray_end()
{
	if (g_card_depth <= 0)
		return;
	--g_card_depth;
	const float pad = g_card_depth < kMaxCards ? g_cards[g_card_depth].pad : 0.0f;
	const float spacing = ImGui::GetStyle().ItemSpacing.y;
	ImGui::Unindent(pad);
	const ImVec2 c = ImGui::GetCursorScreenPos();
	const float bottom = c.y - spacing + 4.0f * unit();
	if (g_card_depth < kMaxCards)
		ImGui::GetStateStorage()->SetFloat(g_cards[g_card_depth].height_id, bottom - g_cards[g_card_depth].min.y);
	ImGui::SetCursorScreenPos(ImVec2(c.x, bottom));
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

bool disclosure(const char *id, const char *title)
{
	const float k = unit();
	ImGuiStorage *st = ImGui::GetStateStorage();
	const ImGuiID sid = ImGui::GetID(id);
	bool open = st->GetInt(sid, 0) != 0;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = avail_width();
	const float h = ImGui::GetFrameHeight();
	ImGui::PushID(id);
	const bool pressed = ImGui::InvisibleButton("##disclosure", ImVec2(w, h));
	const bool hovered = ImGui::IsItemHovered();
	ImGui::PopID();
	if (pressed) {
		open = !open;
		st->SetInt(sid, open ? 1 : 0);
	}
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 q(p.x + w, p.y + h);
	const float r = 5.0f * k;
	dl->AddRectFilled(p, q, col(hovered ? mix(g_pal.surface_hi, g_pal.text, 0.05f) : g_pal.surface_hi), r,
		open ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);
	chevron(dl, ImVec2(p.x + 11.0f * k, p.y + h * 0.5f), open, hovered ? g_pal.text : g_pal.muted);
	dl->AddText(ImVec2(p.x + 22.0f * k, p.y + (h - ImGui::GetFontSize()) * 0.5f), col(g_pal.text), title);
	if (!open)
		return false;

	ImGui::PushID(id);
	const ImGuiID hid = ImGui::GetID("##disclosure_height");
	ImGui::PopID();
	const float bh = st->GetFloat(hid, 0.0f);
	const float t = hairline();
	if (bh > 0.0f) {
		const ImVec2 a(p.x, q.y), b(q.x, q.y + bh);
		dl->AddRectFilled(a, b, col(g_pal.tray), r, ImDrawFlags_RoundCornersBottom);
		dl->AddRect(ImVec2(a.x, a.y - h), b, col(g_pal.border), r, ImDrawFlags_RoundCornersAll, t);
	}
	const float pad = card_pad();
	if (g_card_depth < kMaxCards)
		g_cards[g_card_depth] = Card{ ImVec2(p.x, q.y), q.x, pad, hid };
	++g_card_depth;
	ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + pad - ImGui::GetStyle().ItemSpacing.y));
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
	ImGui::Indent(pad);
	ImGui::PushTextWrapPos(q.x - pad - ImGui::GetWindowPos().x + ImGui::GetScrollX());
	return true;
}

void disclosure_end()
{
	if (g_card_depth <= 0)
		return;
	--g_card_depth;
	const float pad = g_card_depth < kMaxCards ? g_cards[g_card_depth].pad : card_pad();
	const ImGuiStyle &style = ImGui::GetStyle();
	ImGui::PopTextWrapPos();
	ImGui::Unindent(pad);
	const ImVec2 c = ImGui::GetCursorScreenPos();
	const float bottom = c.y - style.ItemSpacing.y + pad;
	if (g_card_depth < kMaxCards)
		ImGui::GetStateStorage()->SetFloat(g_cards[g_card_depth].height_id, bottom - g_cards[g_card_depth].min.y);
	ImGui::SetCursorScreenPos(ImVec2(c.x, bottom));
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void subsection(const char *title, const char *aside)
{
	const float k = unit();
	const std::string upper_title = upper_case(title);
	const char *upper = upper_title.c_str();
	const float tracking = 1.0f * k;
	ImGui::Dummy(ImVec2(0.0f, 6.0f * k));
	const ImVec2 a = ImGui::GetCursorScreenPos();
	const float tw = tracked_width(upper, tracking);
	const float th = ImGui::GetTextLineHeight();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	tracked_text(dl, a.x, a.y, ink(g_subsection), upper, tracking);
	ImGui::Dummy(ImVec2(tw, th));
	const float y = std::floor(a.y + th * 0.5f + 0.5f * k);
	const float x0 = a.x + tw + 10.0f * k;
	float x1 = a.x + avail_width();
	bool aside_under = false;
	if (aside != nullptr && *aside != '\0') {
		const float aw = ImGui::CalcTextSize(aside).x;
		if (x1 - aw - 10.0f * k > x0 + 20.0f * k) {
			dl->AddText(ImVec2(x1 - aw, a.y), col(ink(g_pal.muted)), aside);
			x1 -= aw + 10.0f * k;
		} else {
			aside_under = true;
		}
	}
	if (x1 > x0)
		dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + hairline()), col(g_pal.border));
	if (aside_under)
		wrapped(g_pal.muted, aside);
}

namespace {

Card *section_card()
{
	for (int i = std::min(g_card_depth, kMaxCards) - 1; i >= 0; --i)
		if (g_cards[i].stack_id != 0)
			return &g_cards[i];
	return nullptr;
}

}

bool stacked_captions()
{
	const Card *c = section_card();
	return c != nullptr && c->stack;
}

void need_stacked_captions()
{
	if (Card *c = section_card())
		c->stack_next = true;
}

bool toggle(const char *label, bool *value)
{
	const float k = unit();
	const ImGuiStyle &style = ImGui::GetStyle();
	const char *label_end = visible_end(label);
	const float label_w = ImGui::CalcTextSize(label, label_end).x;
	const float fh = ImGui::GetFrameHeight();
	const float sh = ImGui::GetFontSize() + 3.0f * k;
	const float sw = sh * 1.8f;
	const float total = sw + (label_w > 0.0f ? style.ItemInnerSpacing.x + 3.0f * k + label_w : 0.0f);

	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImGuiID id = ImGui::GetID(label);
	const bool pressed = ImGui::InvisibleButton(label, ImVec2(total, fh));
	if (pressed)
		*value = !*value;
	const bool hovered = ImGui::IsItemHovered();

	ImGuiStorage *st = ImGui::GetStateStorage();
	const float target = *value ? 1.0f : 0.0f;
	float t = st->GetFloat(id, target);
	t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
	if (std::abs(target - t) < 0.01f)
		t = target;
	st->SetFloat(id, t);

	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float y0 = p.y + (fh - sh) * 0.5f;
	const ImVec2 a(p.x, y0), b(p.x + sw, y0 + sh);
	const float rounding = sh * 0.5f;
	const float r_on = sh * 0.5f - 2.5f * k;
	const float cx = p.x + sh * 0.5f + t * (sw - sh);
	const ImVec2 knob(cx, y0 + sh * 0.5f);
	if (g_dim > 0) {
		const ImVec4 edge = undimmed(switch_edge(false));
		dl->AddRect(a, b, col(edge), rounding, 0, std::max(1.0f, std::floor(1.5f * k + 0.5f)));
		dl->AddCircleFilled(knob, r_on - 1.5f * k, col(edge), 24);
		if (label_w > 0.0f)
			dl->AddText(ImVec2(p.x + sw + style.ItemInnerSpacing.x + 3.0f * k, p.y + style.FramePadding.y),
				col(ink(g_pal.text)), label, label_end);
		return pressed;
	}
	if (hovered && t < 1.0f)
		dl->AddRectFilled(a, b, col(with_alpha(g_pal.text, 0.06f * (1.0f - t))), rounding);
	if (t > 0.0f) {
		const int first = dl->VtxBuffer.Size;
		dl->AddRectFilled(a, b, col(with_alpha(switch_on(hovered), t)), rounding);
		shade(dl, first, a, ImVec2(b.x, a.y), switch_on(hovered), switch_on(hovered, true));
	}
	if (t < 1.0f)
		dl->AddRect(a, b, col(with_alpha(switch_edge(hovered), 1.0f - t)), rounding, 0,
			std::max(1.0f, std::floor(1.5f * k + 0.5f)));
	const float r = r_on - (1.0f - t) * 1.5f * k;
	if (t > 0.0f)
		dl->AddCircleFilled(ImVec2(knob.x, knob.y + 0.75f * k), r + 0.5f * k,
			col(ImVec4(0.0f, 0.0f, 0.0f, (g_dark ? 0.30f : 0.22f) * t)), 24);
	dl->AddCircleFilled(knob, r, col(mix(switch_edge(hovered), knob_on(), t)), 24);
	if (label_w > 0.0f)
		dl->AddText(ImVec2(p.x + sw + style.ItemInnerSpacing.x + 3.0f * k, p.y + style.FramePadding.y),
			col(ink(g_pal.text)), label, label_end);
	return pressed;
}

float control_width()
{
	const float column = std::max(g_caption_fit, ImGui::CalcTextSize("0").x * kCaptionChars);
	return std::max(1.0f, avail_width() - column - ImGui::GetStyle().ItemInnerSpacing.x);
}

void note_caption(float width)
{
	g_caption_next = std::max(g_caption_next, width);
}

namespace {

float value_pad()
{
	return 8.0f * unit();
}

ImVec4 slider_fill(bool end = false)
{
	return end ? g_pal.accent_mark_end : g_pal.accent_mark;
}

struct SliderLook {
	ImGuiID edit_id = 0;
	bool editing = false;
	const char *use = nullptr;
	char id[512]{};
};

void slider_begin(SliderLook &s, const char *label, float frac, float zero, const char *shown, float widest,
	float grab_min, float int_range)
{
	const float k = unit();
	const ImGuiStyle &style = ImGui::GetStyle();
	const char *end = visible_end(label);
	const bool captioned = end != label;
	g_value_next = std::max(g_value_next, widest);
	const float value_w = std::max(g_value_fit, widest) + value_pad() * 2.0f;
	s.use = label;
	const bool beside = captioned && !stacked_captions();
	if (captioned) {
		note_caption(ImGui::CalcTextSize(label, end).x);
		if (control_width() < value_w + ImGui::GetFrameHeight() * 4.0f)
			need_stacked_captions();
	}
	if (captioned && !beside) {
		ImGui::TextUnformatted(label, end);
		const ImVec2 c = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(c.x, c.y - style.ItemSpacing.y + 2.0f * k));
		std::snprintf(s.id, sizeof(s.id), "##%s", label);
		s.use = s.id;
	}
	const float w = beside ? control_width() : avail_width();
	const float h = ImGui::GetFrameHeight();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImVec2 q(p.x + w, p.y + h);
	const float rail_w = std::max(h * 2.0f, w - value_w);
	ImGui::PushID(s.use);
	s.edit_id = ImGui::GetID("##editing");
	ImGui::PopID();
	s.editing = ImGui::GetStateStorage()->GetInt(s.edit_id, 0) != 0;

	ImDrawList *dl = ImGui::GetWindowDrawList();
	const bool hovered = ImGui::IsMouseHoveringRect(p, q, true);
	dl->AddRectFilled(p, q, col(hovered ? mix(g_pal.surface_hi, g_pal.text, 0.05f) : g_pal.surface_hi),
		style.FrameRounding);
	if (!s.editing) {
		const ImVec2 ts = ImGui::CalcTextSize(shown);
		dl->AddText(ImVec2(q.x - value_pad() - ts.x, p.y + style.FramePadding.y), col(ink(g_pal.text)), shown);
		if (beside)
			dl->AddText(ImVec2(q.x + style.ItemInnerSpacing.x, p.y + style.FramePadding.y), col(ink(g_pal.text)),
				label, end);

		const float slider_sz = rail_w - 4.0f;
		float grab = grab_min;
		if (int_range > 0.0f)
			grab = std::max(slider_sz / (int_range + 1.0f), grab_min);
		grab = std::min(grab, slider_sz);
		const float x0 = p.x + 2.0f + grab * 0.5f, x1 = p.x + rail_w - 2.0f - grab * 0.5f;
		const float t = std::max(2.0f, std::floor(3.5f * k));
		const float y0 = std::floor(p.y + (h - t) * 0.5f), y1 = y0 + t;
		const float cy = (y0 + y1) * 0.5f;
		const float at = x0 + (x1 - x0) * std::clamp(frac, 0.0f, 1.0f);
		const float from = x0 + (x1 - x0) * std::clamp(zero, 0.0f, 1.0f);
		dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col(slider_groove()), t * 0.5f);
		if (std::abs(at - from) >= 1.0f) {
			const int first = dl->VtxBuffer.Size;
			dl->AddRectFilled(ImVec2(std::min(at, from), y0), ImVec2(std::max(at, from), y1), col(slider_fill()),
				t * 0.5f);
			shade(dl, first, ImVec2(x0, y0), ImVec2(x1, y0), slider_fill(), slider_fill(true));
		}
		const float knob = 5.0f * k;
		if (g_dark) {
			dl->AddCircleFilled(ImVec2(at, cy + 0.75f * k), knob + 0.75f * k, col(ImVec4(0.0f, 0.0f, 0.0f, 0.40f)), 24);
			dl->AddCircleFilled(ImVec2(at, cy), knob, col(g_pal.knob), 24);
		} else {
			dl->AddCircleFilled(ImVec2(at, cy + 0.75f * k), knob + 0.5f * k, col(ImVec4(0.0f, 0.0f, 0.0f, 0.12f)), 24);
			dl->AddCircleFilled(ImVec2(at, cy), knob, col(g_pal.knob), 24);
			const float along = x1 > x0 ? std::clamp((at - x0) / (x1 - x0), 0.0f, 1.0f) : 0.0f;
			dl->AddCircle(ImVec2(at, cy), knob - 0.75f * k, col(mix(slider_fill(), slider_fill(true), along)), 24,
				1.5f * k);
		}
	}

	ImGui::SetNextItemWidth(rail_w);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, with_alpha(g_pal.text, 0.04f));
	ImGui::PushStyleColor(ImGuiCol_Text, s.editing ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
		ImVec2(value_w + style.ItemInnerSpacing.x, style.ItemInnerSpacing.y));
}

void slider_end(const SliderLook &s)
{
	ImGui::PopStyleVar(1);
	ImGui::PopStyleColor(4);
	const bool active = ImGui::IsItemActive();
	ImGui::GetStateStorage()->SetInt(s.edit_id,
		active && (s.editing || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) ? 1 : 0);
}

float zero_of(float min, float max)
{
	return min < 0.0f && max > 0.0f ? -min / (max - min) : 0.0f;
}

}

bool slider_float(const char *label, float *value, float min, float max, const char *format, ImGuiSliderFlags flags)
{
	char shown[64], lo[64], hi[64];
	std::snprintf(shown, sizeof(shown), format, static_cast<double>(*value));
	std::snprintf(lo, sizeof(lo), format, static_cast<double>(min));
	std::snprintf(hi, sizeof(hi), format, static_cast<double>(max));
	const float widest = std::max({ ImGui::CalcTextSize(shown).x, ImGui::CalcTextSize(lo).x, ImGui::CalcTextSize(hi).x });
	SliderLook s;
	slider_begin(s, label, max > min ? (*value - min) / (max - min) : 0.0f, zero_of(min, max), shown, widest,
		ImGui::GetStyle().GrabMinSize, 0.0f);
	const bool changed = ImGui::SliderFloat(s.use, value, min, max, format, flags);
	slider_end(s);
	return changed;
}

bool slider_int(const char *label, int *value, int min, int max, const char *format)
{
	char shown[64], lo[64], hi[64];
	std::snprintf(shown, sizeof(shown), format, *value);
	std::snprintf(lo, sizeof(lo), format, min);
	std::snprintf(hi, sizeof(hi), format, max);
	const float widest = std::max({ ImGui::CalcTextSize(shown).x, ImGui::CalcTextSize(lo).x, ImGui::CalcTextSize(hi).x });
	SliderLook s;
	slider_begin(s, label, max > min ? static_cast<float>(*value - min) / static_cast<float>(max - min) : 0.0f,
		zero_of(static_cast<float>(min), static_cast<float>(max)), shown, widest, ImGui::GetStyle().GrabMinSize,
		static_cast<float>(max - min));
	const bool changed = ImGui::SliderInt(s.use, value, min, max, format, 0);
	slider_end(s);
	return changed;
}

bool button(const char *icon_glyph, const char *label)
{
	if (!g_icons || icon_glyph == nullptr)
		return ImGui::Button(label);
	char text[1024];
	std::snprintf(text, sizeof(text), "%s  %s###%s", icon_glyph, label, label);
	return ImGui::Button(text);
}

float button_width(const char *icon_glyph, const char *label)
{
	float w = ImGui::CalcTextSize(label, nullptr, true).x;
	if (g_icons && icon_glyph != nullptr)
		w += ImGui::CalcTextSize(icon_glyph).x + ImGui::CalcTextSize("  ").x;
	return w + ImGui::GetStyle().FramePadding.x * 2.0f;
}

void same_line_if_fits(float w)
{
	const float right = ImGui::GetCursorScreenPos().x + avail_width();
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	if (ImGui::GetItemRectMax().x + gap + w <= right)
		ImGui::SameLine(0.0f, gap);
}

bool segment(const char *label, bool selected, float width, ImDrawFlags corners, bool divider, Brand brand,
	bool lit, bool framed, bool halo)
{
	const ImVec4 *tint = brand_colour(brand);
	const float k = unit();
	const float h = ImGui::GetFrameHeight();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton("##segment", ImVec2(width, h));
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float r = ImGui::GetStyle().FrameRounding;
	const ImVec2 q(p.x + width, p.y + h);
	if (framed)
		dl->AddRectFilled(p, q, col(g_pal.surface_hi), r, corners);
	const float in = 2.0f * k;
	const ImVec2 a(p.x + in, p.y + in), b(q.x - in, q.y - in);
	const float ir = r - in * 0.5f;
	const float t = hairline();
	const bool maker = selected && tint != nullptr && lit;
	const bool chosen = selected && brand == Brand::Neutral && g_dim == 0;
	const bool faded_tint = selected && g_dim > 0 && tint != nullptr && !g_dark;
	if (maker) {
		if (halo) {
			const float g = in * 0.5f;
			dl->AddRectFilled(ImVec2(a.x - in, a.y - in), ImVec2(b.x + in, b.y + in),
				col(with_alpha(*tint, kHaloOuter)), ir + in);
			dl->AddRectFilled(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g),
				col(with_alpha(*tint, kHaloInner)), ir + g);
		}
		dl->AddRectFilled(a, b, col(*tint), ir);
		dl->AddRect(a, b, col(rim(tint)), ir, 0, t);
	} else if (selected && g_dim > 0) {
		if (faded_tint)
			dl->AddRectFilled(a, b, col(ImVec4(tint->x, tint->y, tint->z, kUnlitTint / kDimAlpha)), ir);
		dl->AddRect(a, b, col(tint != nullptr ? undimmed(rim(tint)) : dim_text()), ir, 0, t);
	} else if (chosen) {
		accent_chip(dl, a, b, ir);
	} else if (selected) {
		dl->AddRectFilled(ImVec2(a.x, a.y + t), ImVec2(b.x, b.y + t),
			col(ImVec4(0.0f, 0.0f, 0.0f, g_dark ? 0.30f : 0.10f)), ir);
		dl->AddRectFilled(a, b, col(raised_fill(tint)), ir);
		dl->AddRect(a, b, col(rim(tint)), ir, 0, t);
	} else if (hovered) {
		dl->AddRectFilled(a, b, col(tint != nullptr ? with_alpha(*tint, held ? 0.24f : 0.16f)
			: with_alpha(g_pal.text, held ? 0.12f : 0.07f)), ir);
	} else if (divider) {
		dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.28f), ImVec2(p.x + t, q.y - h * 0.28f), col(g_pal.border));
	}
	if (maker || chosen)
		dl->AddRectFilled(ImVec2(a.x + ir, a.y + t), ImVec2(b.x - ir, a.y + t * 2.0f),
			col(ImVec4(1.0f, 1.0f, 1.0f, g_dark ? 0.24f : 0.20f)));
	else if (selected && g_dim == 0 && g_dark)
		dl->AddRectFilled(ImVec2(a.x + ir, a.y + t), ImVec2(b.x - ir, a.y + t * 2.0f),
			col(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)));
	const char *end = visible_end(label);
	const ImVec2 s = ImGui::CalcTextSize(label, end);
	const ImVec4 tc = maker ? readable_on(*tint)
		: chosen ? g_chosen_ink
		: selected ? g_pal.text
		: hovered ? g_pal.text : mix(g_pal.text, g_pal.surface_hi, 0.22f);
	const ImVec4 shown = faded_tint ? undimmed(g_tint_faded[rim_slot(tint)]) : ink(tc);
	dl->AddText(ImVec2(p.x + (width - s.x) * 0.5f, p.y + (h - s.y) * 0.5f), col(shown), label, end);
	return pressed;
}

void segment_frame(const ImVec2 &min, const ImVec2 &max, float line_h, int lines)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(min, max, col(g_pal.surface_hi), ImGui::GetStyle().FrameRounding);
	const float t = hairline();
	const float in = 6.0f * unit();
	for (int i = 1; i < lines; ++i) {
		const float y = std::floor(min.y + line_h * static_cast<float>(i) - t * 0.5f);
		dl->AddRectFilled(ImVec2(min.x + in, y), ImVec2(max.x - in, y + t), col(g_pal.border));
	}
}

void segment_glow(const ImVec2 &min, const ImVec2 &max, const ImVec4 &colour)
{
	const float k = unit();
	const float in = 2.0f * k;
	const float ir = ImGui::GetStyle().FrameRounding - in * 0.5f;
	const ImVec2 a(min.x + in, min.y + in), b(max.x - in, max.y - in);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float alpha = g_dark ? 0.05f : 0.035f;
	for (int i = 4; i >= 1; --i) {
		const float e = 1.5f * k * static_cast<float>(i);
		dl->AddRectFilled(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e), col(with_alpha(colour, alpha)), ir + e);
	}
}

void pill(const ImVec4 &colour, const char *text, bool wrap)
{
	const float fh = ImGui::GetFrameHeight();
	const float room = avail_width();
	const float max_w = wrap && pill_size(text).x > room ? room : 0.0f;
	const ImVec2 s = pill_size(text, max_w);
	const float lead = (fh - pill_line()) * 0.5f;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	pill_at(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + lead), colour, text, max_w);
	ImGui::Dummy(ImVec2(s.x, std::max(fh, s.y + lead * 2.0f)));
}

float pill_width(const char *text)
{
	return pill_size(text).x;
}

void icon_text(const ImVec4 &colour, const char *glyph)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImVec2 s = ImGui::CalcTextSize(glyph);
	ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + icon_drop()), col(ink(colour)), glyph);
	ImGui::Dummy(s);
}

void brand_chip(Brand b, const char *text, bool lit)
{
	const float fh = ImGui::GetFrameHeight();
	const ImVec2 s = chip_size(text);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	chip_at(ImGui::GetWindowDrawList(), ImVec2(p.x, std::floor(p.y + (fh - s.y) * 0.5f)), text, b, lit);
	ImGui::Dummy(ImVec2(s.x, fh));
}

namespace {

struct OsdCard {
	ImVec2 min;
	ImGuiID w_id = 0, h_id = 0;
};
OsdCard g_osd;

float osd_pad()
{
	return 9.0f * unit();
}

}

void osd_begin(const char *id)
{
	const float k = unit();
	ImGui::PushID(id);
	g_osd.w_id = ImGui::GetID("##osd_w");
	g_osd.h_id = ImGui::GetID("##osd_h");
	ImGui::PopID();
	g_osd.min = ImGui::GetCursorScreenPos();
	ImGuiStorage *st = ImGui::GetStateStorage();
	const ImVec2 size(st->GetFloat(g_osd.w_id, 0.0f), st->GetFloat(g_osd.h_id, 0.0f));
	if (size.x > 0.0f && size.y > 0.0f) {
		ImDrawList *dl = ImGui::GetWindowDrawList();
		const ImVec2 a = g_osd.min, b(a.x + size.x, a.y + size.y);
		const float r = card_rounding();
		dl->AddRectFilled(ImVec2(a.x, a.y + 2.0f * k), ImVec2(b.x, b.y + 2.0f * k),
			col(ImVec4(0.0f, 0.0f, 0.0f, g_dark ? 0.35f : 0.10f)), r);
		gradient_rect(dl, a, b, banner_left(), g_pal.surface, r, ImDrawFlags_RoundCornersAll);
		dl->AddRect(a, b, col(g_pal.border), r, 0, hairline());
	}
	const float pad = osd_pad();
	ImGui::SetCursorScreenPos(ImVec2(g_osd.min.x + pad, g_osd.min.y + pad));
	ImGui::BeginGroup();
}

void osd_end()
{
	ImGui::EndGroup();
	const float pad = osd_pad();
	const ImVec2 s = ImGui::GetItemRectSize();
	ImGuiStorage *st = ImGui::GetStateStorage();
	st->SetFloat(g_osd.w_id, std::floor(s.x + pad * 2.0f));
	st->SetFloat(g_osd.h_id, std::floor(s.y + pad * 2.0f));
	ImGui::SetCursorScreenPos(ImVec2(g_osd.min.x, g_osd.min.y + s.y + pad * 2.0f));
	ImGui::Dummy(ImVec2(s.x + pad * 2.0f, 0.0f));
}

void osd_mark()
{
	const float k = unit();
	const float fh = ImGui::GetFrameHeight();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float logo = std::round(std::min(fh, 16.0f * k));
	draw_logo(ImGui::GetWindowDrawList(), ImVec2(std::round(p.x + 1.0f * k), std::round(p.y + (fh - logo) * 0.5f)),
		logo);
	ImGui::Dummy(ImVec2(logo + 2.0f * k, fh));
}

void callout(const ImVec4 &colour, const char *icon_glyph, const char *text)
{
	const float k = unit();
	const float w = avail_width();
	const float pad = 8.0f * k;
	const float bar = 2.0f * k;
	const char *g = g_icons && icon_glyph != nullptr ? icon_glyph : nullptr;
	const float icon_w = g != nullptr ? ImGui::CalcTextSize(g).x + 7.0f * k : 0.0f;
	const float wrap = std::max(10.0f, w - bar - pad * 2.0f - icon_w);
	const Span span{ text, mix(colour, g_pal.text, 0.55f) };
	Layout l;
	lay_out_paragraph(&span, 1, wrap, l);
	const float lh = ImGui::GetTextLineHeight();
	const float vpad = 6.0f * k;
	const float h = lh * static_cast<float>(l.lines.size()) + vpad * 2.0f;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImVec2 q(p.x + w, p.y + h);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float r = 5.0f * k;
	dl->AddRectFilled(p, q, col(with_alpha(colour, 0.11f)), r);
	dl->AddRect(p, q, col(with_alpha(colour, 0.26f)), r, 0, hairline());
	dl->AddRectFilled(p, ImVec2(p.x + bar, q.y), col(colour), r, ImDrawFlags_RoundCornersLeft);
	float x = p.x + bar + pad;
	if (g != nullptr) {
		dl->AddText(ImVec2(x, p.y + vpad + icon_drop()), col(colour), g);
		x += icon_w;
	}
	draw_layout(dl, l, ImVec2(x, p.y + vpad), &span, lh);
	ImGui::Dummy(ImVec2(w, h));
}

void calloutf(const ImVec4 &colour, const char *icon_glyph, const char *fmt, ...)
{
	char text[2048];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	callout(colour, icon_glyph, text);
}

void flow(const Span *spans, int count)
{
	Layout l;
	lay_out_paragraph(spans, count, text_room(), l);
	const float lh = ImGui::GetTextLineHeight();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	draw_layout(ImGui::GetWindowDrawList(), l, p, spans, lh);
	ImGui::Dummy(ImVec2(l.w, lh * static_cast<float>(l.lines.size())));
}

void wrapped(const ImVec4 &colour, const char *text)
{
	const Span span{ text, colour };
	flow(&span, 1);
}

float avail_width()
{
	if (g_card_depth > 0 && g_card_depth <= kMaxCards)
		return std::max(1.0f, g_cards[g_card_depth - 1].right - g_cards[g_card_depth - 1].pad -
			ImGui::GetCursorScreenPos().x);
	return ImGui::GetContentRegionAvail().x;
}

#if AEONSR_PANEL_PREVIEW
std::vector<ContrastPair> contrast_pairs()
{
	const Palette &p = g_pal;
	const ImVec4 window = opaque(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
	const ImVec4 held = held_header();
	const ImVec4 rest = p.header;
	const ImVec4 tray_alt = over(p.tray, p.text, 0.03f);
	std::vector<ContrastPair> out = {
		{ "text on card", p.text, p.surface, 4.5f },
		{ "text on control", p.text, p.surface_hi, 4.5f },
		{ "secondary on card", p.muted, p.surface, 4.5f },
		{ "secondary on window", p.muted, window, 4.5f },
		{ "secondary on control", p.muted, p.surface_hi, 4.5f },
		{ "disclosure chevron", p.muted, p.surface_hi, 3.0f },
		{ "subsection title", g_subsection, p.surface, 4.5f },
		{ "banner title", p.text, banner_left(), 4.5f },
		{ "banner second word", banner_second(), banner_left(), 4.5f },
		{ "banner second word, violet", g_banner_accent[1], banner_left(), 4.5f },
		{ "banner second word on card", banner_second(), p.surface, 4.5f },
		{ "banner second word, violet, on card", g_banner_accent[1], p.surface, 4.5f },
		{ "banner subtitle", p.muted, p.surface, 4.5f },
		{ "banner subtitle stacked", p.muted, banner_left(), 4.5f },
		{ "section title", p.text, held, 4.5f },
		{ "unlit tile icon", p.label, tile_unlit(), 3.0f },
		{ "chevron", p.muted, held, 3.0f },
		{ "unselected option", mix(p.text, p.surface_hi, 0.22f), p.surface_hi, 4.5f },
		{ "chosen option", g_chosen_ink, p.accent, 4.5f },
		{ "chosen rim", g_rim[3], p.surface_hi, 3.0f },
		{ "None chosen label", p.text, p.raised, 4.5f },
		{ "None chosen rim", g_rim[4], p.surface_hi, 3.0f },
		{ "faded option", dim_text(), over(p.surface, p.surface_hi, kDimAlpha), 4.5f },
		{ "faded chosen outline", dim_text(), over(p.surface, p.surface_hi, kDimAlpha), 3.0f },
		{ "faded text on card", dim_text(), p.surface, 4.5f },
		{ "faded text on tray", dim_text(), p.tray, 4.5f },
		{ "slider value", p.text, p.surface_hi, 4.5f },
		{ "slider value hovered", p.text, over(p.surface_hi, p.text, 0.05f), 4.5f },
		{ "slider value held", p.text, over(p.surface_hi, p.text, 0.09f), 4.5f },
		{ "slider fill", slider_fill(), p.surface_hi, 3.0f },
		{ "slider fill, violet", slider_fill(true), p.surface_hi, 3.0f },
		{ "slider fill on groove", slider_fill(), slider_groove(), 3.0f },
		{ "slider fill on groove, violet", slider_fill(true), slider_groove(), 3.0f },
		{ "slider fill hovered", slider_fill(), over(p.surface_hi, p.text, 0.05f), 3.0f },
		{ "slider fill hovered, violet", slider_fill(true), over(p.surface_hi, p.text, 0.05f), 3.0f },
		{ "slider knob", g_dark ? p.knob : slider_fill(), p.surface_hi, 3.0f },
		{ "slider knob, violet", g_dark ? p.knob : slider_fill(true), p.surface_hi, 3.0f },
		{ "slider knob hovered", g_dark ? p.knob : slider_fill(true), over(p.surface_hi, p.text, 0.05f), 3.0f },
		{ "progress caption on fill", p.text, progress_fill(), 4.5f },
		{ "switch on track", switch_on(false), p.surface, 3.0f },
		{ "switch on track, violet", switch_on(false, true), p.surface, 3.0f },
		{ "switch on track hovered", switch_on(true), p.surface, 3.0f },
		{ "switch on track hovered, violet", switch_on(true, true), p.surface, 3.0f },
		{ "switch on track on tray", switch_on(false), p.tray, 3.0f },
		{ "switch on track on tray, violet", switch_on(false, true), p.tray, 3.0f },
		{ "switch knob on", knob_on(), switch_on(false), 3.0f },
		{ "switch knob on, violet", knob_on(), switch_on(false, true), 3.0f },
		{ "switch knob on hovered", knob_on(), switch_on(true), 3.0f },
		{ "switch knob on hovered, violet", knob_on(), switch_on(true, true), 3.0f },
		{ "switch off outline and knob", switch_edge(false), p.surface, 3.0f },
		{ "switch off hovered", switch_edge(true), over(p.surface, p.text, 0.06f), 3.0f },
		{ "switch off on tray", switch_edge(false), p.tray, 3.0f },
		{ "faded switch on card", switch_edge(false), p.surface, 3.0f },
		{ "faded switch on band", switch_edge(false), p.band, 3.0f },
		{ "text on band", p.text, p.band, 4.5f },
		{ "secondary on band", p.muted, p.band, 4.5f },
		{ "faded text on band", dim_text(), p.band, 4.5f },
		{ "switch on track on band", switch_on(false), p.band, 3.0f },
		{ "switch on track on band, violet", switch_on(false, true), p.band, 3.0f },
		{ "switch off on band", switch_edge(false), p.band, 3.0f },
		{ "tag outline, not running", g_chip_edge, rest, 3.0f },
		{ "tag outline, not running, hovered", g_chip_edge, over(rest, p.text, 0.025f), 3.0f },
		{ "tag outline, not running, held", g_chip_edge, held, 3.0f },
		{ "tag outline, not running, on-screen card", g_chip_edge, p.surface, 3.0f },
		{ "tag name, not running", g_chip_text, rest, 4.5f },
		{ "tag name, not running, hovered", g_chip_text, over(rest, p.text, 0.025f), 4.5f },
		{ "tag name, not running, held", g_chip_text, held, 4.5f },
		{ "tag name, not running, on-screen card", g_chip_text, p.surface, 4.5f },
		{ "tag name, not running, on-screen card left", g_chip_text, banner_left(), 4.5f },
		{ "on-screen name", p.text, banner_left(), 4.5f },
		{ "on-screen row name", p.muted, banner_left(), 4.5f },
		{ "on-screen row name, right", p.muted, p.surface, 4.5f },
		{ "text on tray", p.text, p.tray, 4.5f },
		{ "secondary on tray", p.muted, p.tray, 4.5f },
		{ "subsection title on tray", g_subsection, p.tray, 4.5f },
		{ "disclosure chevron hovered", p.text, over(p.surface_hi, p.text, 0.05f), 3.0f },
		{ "text on tray alt row", p.text, tray_alt, 4.5f },
		{ "secondary on tray alt row", p.muted, tray_alt, 4.5f },
		{ "narrow row name on tray alt row", p.label, tray_alt, 4.5f },
	};
	const struct { const char *name; ImVec4 c; } status[] = {
		{ "ok", p.ok }, { "warn", p.warn }, { "fail", p.fail }, { "off", p.off }, { "loading", p.loading },
	};
	for (const auto &n : status) {
		const std::string name = n.name;
		const bool loud = alert(n.c);
		const ImVec4 on_card = loud ? over(p.surface, n.c, kPillFill) : over(p.surface, p.text, kCalmFill);
		const ImVec4 on_window = loud ? over(window, n.c, kPillFill) : over(window, p.text, kCalmFill);
		out.push_back({ name + " pill text", pill_text_colour(n.c), on_card, 4.5f });
		out.push_back({ name + " pill dot", n.c, on_card, 3.0f });
		out.push_back({ name + " pill text on window", pill_text_colour(n.c), on_window, 4.5f });
		out.push_back({ name + " pill dot on window", n.c, on_window, 3.0f });
		const ImVec4 on_band = loud ? over(p.band, n.c, kPillFill) : over(p.band, p.text, kCalmFill);
		const ImVec4 on_osd = loud ? over(banner_left(), n.c, kPillFill) : over(banner_left(), p.text, kCalmFill);
		out.push_back({ name + " pill text on band", pill_text_colour(n.c), on_band, 4.5f });
		out.push_back({ name + " pill mark on band", n.c, on_band, 3.0f });
		out.push_back({ name + " pill text on-screen", pill_text_colour(n.c), on_osd, 4.5f });
		out.push_back({ name + " pill mark on-screen", n.c, on_osd, 3.0f });
		out.push_back({ name + " callout text", mix(n.c, p.text, 0.55f), over(p.surface, n.c, 0.11f), 4.5f });
		out.push_back({ name + " as icon", n.c, p.surface, 3.0f });
		out.push_back({ name + " as icon on tray", n.c, tray_alt, 3.0f });
	}
	const struct { const char *name; const ImVec4 *c; Brand b; } brand[] = {
		{ "DLSS", &p.dlss, Brand::Nvidia }, { "FSR", &p.fsr, Brand::Amd }, { "XeSS", &p.xess, Brand::Intel },
	};
	const ImVec4 faded_floor = over(p.surface, p.surface_hi, kDimAlpha);
	for (const auto &n : brand) {
		const std::string name = n.name;
		const ImVec4 &c = *n.c;
		out.push_back({ name + " selected option", readable_on(c), c, 4.5f });
		out.push_back({ name + " fill rim", rim(n.c), over(p.surface_hi, c, halo_share()), 3.0f });
		out.push_back({ name + " rim, not running", rim(n.c), p.surface_hi, 3.0f });
		out.push_back({ name + " outline, faded row", rim(n.c), faded_floor, 3.0f });
		out.push_back({ name + " chosen label, not running", p.text, raised_fill(n.c), 4.5f });
		out.push_back({ name + " chosen label, faded row", g_dark ? dim_text() : g_tint_faded[rim_slot(n.c)],
			g_dark ? faded_floor : over(faded_floor, c, kUnlitTint), 4.5f });
		out.push_back({ name + " hover", p.text, over(p.surface_hi, c, 0.24f), 4.5f });
		out.push_back({ name + " tile icon", tile_glyph(c), tile_lit(c, rest), 3.0f });
		out.push_back({ name + " tile icon pressed", tile_glyph(c), tile_lit(c, held), 3.0f });
		out.push_back({ name + " name chip", readable_on(c), c, 4.5f });
		out.push_back({ name + " name chip rim on header", rim(n.c), rest, 3.0f });
		out.push_back({ name + " name chip rim on held header", rim(n.c), held, 3.0f });
		out.push_back({ name + " name chip rim on-screen", rim(n.c), p.surface, 3.0f });
		out.push_back({ name + " name chip rim on-screen left", rim(n.c), banner_left(), 3.0f });
	}
	out.push_back({ "warn pill on header", mix(p.warn, p.text, 0.3f), over(held, p.warn, kPillFill), 4.5f });
	out.push_back({ "fail pill on header", mix(p.fail, p.text, 0.3f), over(held, p.fail, kPillFill), 4.5f });
	out.push_back({ "warn mark on header", p.warn, over(held, p.warn, kPillFill), 3.0f });
	out.push_back({ "fail mark on header", p.fail, over(held, p.fail, kPillFill), 3.0f });
	out.push_back({ "fail mark on header, rest", p.fail, over(rest, p.fail, kPillFill), 3.0f });
	out.push_back({ "fail as text", p.fail, p.surface, 4.5f });
	out.push_back({ "warn as text", p.warn, p.surface, 4.5f });
	for (ContrastPair &c : out)
		c.bg = over(window, c.bg, c.bg.w);
	return out;
}
#endif

}
}

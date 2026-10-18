#include "aeon_sr/core/i18n.hpp"

#include <array>
#include <atomic>
#include <cstring>

namespace aeon_sr {
namespace i18n {
namespace {

struct Source {
	const char *id;
	const char *text;
	const char *context;
};

const Source kSource[] = {
#define AEON_STR(id, text, context) { #id, text, context },
#include "aeon_sr/core/i18n_strings.inc"
#undef AEON_STR
};
static_assert(sizeof(kSource) / sizeof(kSource[0]) == kStringCount, "one source line per id");

struct Entry {
	S id;
	const char *text;
};

#define AEON_TR(id, text) { S::id, text },
const Entry kArSA[] = {
#include "i18n/ar_SA.inc"
};
const Entry kBgBG[] = {
#include "i18n/bg_BG.inc"
};
const Entry kDeDE[] = {
#include "i18n/de_DE.inc"
};
const Entry kEsES[] = {
#include "i18n/es_ES.inc"
};
const Entry kFrFR[] = {
#include "i18n/fr_FR.inc"
};
const Entry kJaJP[] = {
#include "i18n/ja_JP.inc"
};
const Entry kKoKR[] = {
#include "i18n/ko_KR.inc"
};
const Entry kPlPL[] = {
#include "i18n/pl_PL.inc"
};
const Entry kPtBR[] = {
#include "i18n/pt_BR.inc"
};
const Entry kRuRU[] = {
#include "i18n/ru_RU.inc"
};
const Entry kSlSI[] = {
#include "i18n/sl_SI.inc"
};
const Entry kThTH[] = {
#include "i18n/th_TH.inc"
};
const Entry kTrTR[] = {
#include "i18n/tr_TR.inc"
};
const Entry kZhCN[] = {
#include "i18n/zh_CN.inc"
};
const Entry kZhHK[] = {
#include "i18n/zh_HK.inc"
};
#undef AEON_TR

struct LangInfo {
	const char *code;
	const Entry *entries;
	size_t count;
	unsigned int glyph;
};

template <size_t N>
constexpr LangInfo info(const char *code, const Entry (&e)[N], unsigned int glyph)
{
	return { code, e, N, glyph };
}

const LangInfo kLangs[kLangCount] = {
	{ "en-US", nullptr, 0, 0 },
	info("ar-SA", kArSA, 0xFECB),
	info("bg-BG", kBgBG, 0x0414),
	info("de-DE", kDeDE, 0),
	info("es-ES", kEsES, 0),
	info("fr-FR", kFrFR, 0),
	info("ja-JP", kJaJP, 0x306E),
	info("ko-KR", kKoKR, 0xD55C),
	info("pl-PL", kPlPL, 0x0142),
	info("pt-BR", kPtBR, 0),
	info("ru-RU", kRuRU, 0x0414),
	info("sl-SI", kSlSI, 0x010D),
	info("th-TH", kThTH, 0x0E01),
	info("tr-TR", kTrTR, 0x011F),
	info("zh-CN", kZhCN, 0x8BBE),
	info("zh-HK", kZhHK, 0x8A2D),
};

struct Tables {
	std::array<std::array<const char *, kStringCount>, kLangCount> text{};
	std::array<std::array<unsigned char, kStringCount>, kLangCount> count{};

	Tables()
	{
		for (unsigned int l = 0; l < kLangCount; ++l) {
			for (size_t i = 0; i < kLangs[l].count; ++i) {
				const unsigned int s = static_cast<unsigned int>(kLangs[l].entries[i].id);
				if (count[l][s] == 0)
					text[l][s] = kLangs[l].entries[i].text;
				if (count[l][s] < 255)
					++count[l][s];
			}
		}
	}
};

const Tables &tables()
{
	static const Tables t;
	return t;
}

std::atomic<unsigned char> g_current{ 0 };

unsigned int index(Lang lang) noexcept
{
	const unsigned int l = static_cast<unsigned int>(lang);
	return l < kLangCount ? l : 0u;
}

char fold(char c) noexcept
{
	if (c == '_')
		return '-';
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool ends_sentence(const std::string &s)
{
	if (s.empty())
		return false;
	const char last = s.back();
	if (last == '.' || last == '!' || last == '?')
		return true;
	return s.size() >= 3 && ((s.compare(s.size() - 3, 3, "\xE3\x80\x82") == 0) ||
		(s.compare(s.size() - 3, 3, "\xEF\xBC\x8E") == 0));
}

}

const char *lang_code(Lang lang) noexcept
{
	return kLangs[index(lang)].code;
}

bool lang_from_code(std::string_view code, Lang *out) noexcept
{
	for (unsigned int l = 0; l < kLangCount; ++l) {
		const char *c = kLangs[l].code;
		if (std::strlen(c) != code.size())
			continue;
		bool same = true;
		for (size_t i = 0; i < code.size() && same; ++i)
			same = fold(code[i]) == fold(c[i]);
		if (same) {
			*out = static_cast<Lang>(l);
			return true;
		}
	}
	return false;
}

Lang resolve(std::string_view configured, const std::vector<std::string> &ui_languages) noexcept
{
	Lang lang = Lang::en_US;
	if (!configured.empty())
		return lang_from_code(configured, &lang) ? lang : Lang::en_US;
	for (const std::string &l : ui_languages)
		if (lang_from_code(l, &lang))
			return lang;
	return Lang::en_US;
}

const char *english(S id) noexcept
{
	const unsigned int s = static_cast<unsigned int>(id);
	return s < kStringCount ? kSource[s].text : "";
}

const char *stored(Lang lang, S id) noexcept
{
	const unsigned int s = static_cast<unsigned int>(id);
	if (s >= kStringCount)
		return nullptr;
	if (lang == Lang::en_US)
		return kSource[s].text;
	return tables().text[index(lang)][s];
}

const char *text(Lang lang, S id) noexcept
{
	const char *t = stored(lang, id);
	return t != nullptr ? t : english(id);
}

unsigned int entry_count(Lang lang, S id) noexcept
{
	const unsigned int s = static_cast<unsigned int>(id);
	if (s >= kStringCount)
		return 0;
	if (lang == Lang::en_US)
		return 1;
	return tables().count[index(lang)][s];
}

const char *id_name(S id) noexcept
{
	const unsigned int s = static_cast<unsigned int>(id);
	return s < kStringCount ? kSource[s].id : "";
}

const char *context(S id) noexcept
{
	const unsigned int s = static_cast<unsigned int>(id);
	return s < kStringCount ? kSource[s].context : "";
}

Lang current() noexcept
{
	return static_cast<Lang>(g_current.load(std::memory_order_relaxed));
}

void set_current(Lang lang) noexcept
{
	g_current.store(static_cast<unsigned char>(index(lang)), std::memory_order_relaxed);
}

const std::vector<MessageEntry> &message_entries()
{
	static const std::vector<MessageEntry> kEntries = {
		{ S::msg_fg_starting, false },
		{ S::msg_fg_vsync, false },
		{ S::msg_fg_swap_chain_off, true },
		{ S::msg_fg_not_own_window, true },
		{ S::msg_fg_xess_route, false },
		{ S::msg_flow_nothing_needs, false },
		{ S::msg_flow_warming, false },
		{ S::msg_flow_never_reached, true },
		{ S::msg_jitter_no_upscaler, false },
		{ S::msg_jitter_vsr, false },
		{ S::msg_jitter_restart, false },
		{ S::msg_jitter_api, false },
		{ S::msg_jitter_fsr11, false },
		{ S::msg_jitter_no_gl, false },
		{ S::msg_game_not_taken, false },
		{ S::msg_game_not_drawn, false },
		{ S::msg_game_not_attached, false },
		{ S::msg_game_sets_quality, false },
		{ S::msg_game_sets_quality_other, false },
		{ S::msg_report_failed, true },
	};
	return kEntries;
}

bool lookup(Lang lang, const std::string &english_text, std::string *said, std::string *rest)
{
	rest->clear();
	if (english_text.empty())
		return false;
	std::string bare = english_text;
	const bool stop = bare.back() == '.';
	for (int pass = 0; pass < 2; ++pass) {
		for (const MessageEntry &m : message_entries()) {
			if (m.opening || bare != english(m.id))
				continue;
			*said = text(lang, m.id);
			if (pass == 1 && !ends_sentence(*said))
				*said += '.';
			return true;
		}
		if (!stop)
			break;
		bare.pop_back();
	}
	for (const MessageEntry &m : message_entries()) {
		if (!m.opening)
			continue;
		const char *head = english(m.id);
		const size_t n = std::strlen(head);
		if (english_text.size() >= n && english_text.compare(0, n, head) == 0) {
			*said = text(lang, m.id);
			*rest = english_text.substr(n);
			return true;
		}
	}
	return false;
}

std::string message(Lang lang, const std::string &english_text)
{
	if (lang == Lang::en_US || english_text.empty())
		return english_text;
	std::string said, rest;
	return lookup(lang, english_text, &said, &rest) ? said + rest : english_text;
}

unsigned int sample_glyph(Lang lang) noexcept
{
	return kLangs[index(lang)].glyph;
}

}
}

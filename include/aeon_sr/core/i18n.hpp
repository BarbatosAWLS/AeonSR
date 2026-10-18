#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aeon_sr {
namespace i18n {

enum class Lang : unsigned char {
	en_US, ar_SA, bg_BG, de_DE, es_ES, fr_FR, ja_JP, ko_KR,
	pl_PL, pt_BR, ru_RU, sl_SI, th_TH, tr_TR, zh_CN, zh_HK,
};
constexpr unsigned int kLangCount = 16;

enum class S : unsigned short {
#define AEON_STR(id, text, context) id,
#include "aeon_sr/core/i18n_strings.inc"
#undef AEON_STR
	Count
};
constexpr unsigned int kStringCount = static_cast<unsigned int>(S::Count);

const char *lang_code(Lang lang) noexcept;
bool lang_from_code(std::string_view code, Lang *out) noexcept;

Lang resolve(std::string_view configured, const std::vector<std::string> &ui_languages) noexcept;

const char *text(Lang lang, S id) noexcept;
const char *english(S id) noexcept;
const char *stored(Lang lang, S id) noexcept;
unsigned int entry_count(Lang lang, S id) noexcept;
const char *id_name(S id) noexcept;
const char *context(S id) noexcept;

Lang current() noexcept;
void set_current(Lang lang) noexcept;
inline const char *tr(S id) noexcept { return text(current(), id); }

std::string message(Lang lang, const std::string &english);
inline std::string message(const std::string &english) { return message(current(), english); }
bool lookup(Lang lang, const std::string &english, std::string *said, std::string *rest);
struct MessageEntry {
	S id;
	bool opening;
};
const std::vector<MessageEntry> &message_entries();

unsigned int sample_glyph(Lang lang) noexcept;

}
}

namespace reshade { namespace api { struct effect_runtime; } }

namespace aeon_sr {
namespace i18n {

void follow_reshade(reshade::api::effect_runtime *runtime);

void use_text_shaping(bool on);

}
}

#define TR(id) ::aeon_sr::i18n::tr(::aeon_sr::i18n::S::id)

#include "aeon_sr/core/i18n.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/imgui_reshade.hpp"

#include <Windows.h>

namespace aeon_sr {
namespace i18n {
namespace {

bool read_language(reshade::api::effect_runtime *runtime, std::string *out)
{
	char value[64]{};
	size_t size = sizeof(value);
	if (!reshade::get_config_value(runtime, "OVERLAY", "Language", value, &size))
		return false;
	*out = value;
	return true;
}

std::vector<std::string> ui_languages()
{
	std::vector<std::string> out;
	ULONG num = 0, size = 0;
	if (!GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME | MUI_UI_FALLBACK, &num, nullptr, &size) || size == 0)
		return out;
	std::vector<wchar_t> buf(size);
	if (!GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME | MUI_UI_FALLBACK, &num, buf.data(), &size))
		return out;
	std::string name;
	for (ULONG i = 0; i < size; ++i) {
		const wchar_t c = buf[i];
		if (c == L'\0') {
			if (name.empty())
				break;
			out.push_back(name);
			name.clear();
		} else {
			name.push_back(c < 128 ? static_cast<char>(c) : '?');
		}
	}
	return out;
}

}

void follow_reshade(reshade::api::effect_runtime *runtime)
{
	std::string configured;
	if (!read_language(runtime, &configured) && runtime != nullptr)
		read_language(nullptr, &configured);
	const std::vector<std::string> system = configured.empty() ? ui_languages() : std::vector<std::string>();
	Lang lang = resolve(configured, system);
	bool font_lacks = false;
	if (const unsigned int g = sample_glyph(lang); g != 0) {
		ImFont *const font = ImGui::GetFont();
		font_lacks = font != nullptr && !font->IsGlyphInFont(static_cast<ImWchar>(g));
		if (font_lacks)
			lang = Lang::en_US;
	}
	static Lang logged = static_cast<Lang>(255);
	if (lang != logged) {
		logged = lang;
		std::wstring said = L"panel language: ";
		for (const char *c = lang_code(lang); *c != '\0'; ++c)
			said.push_back(static_cast<wchar_t>(*c));
		said += configured.empty() ? L" (ReShade follows the system)" : L" (ReShade.ini Language=";
		if (!configured.empty()) {
			for (char c : configured)
				said.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
			said += L")";
		}
		if (font_lacks)
			said += L"; ReShade's font has no letters for it, so English";
		diag_info("i18n", said);
	}
	set_current(lang);
	use_text_shaping(lang == Lang::ar_SA);
}

}
}

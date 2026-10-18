#include "aeon_sr/core/bidi.hpp"
#include "aeon_sr/core/i18n.hpp"
#include "aeon_sr/core/imgui_reshade.hpp"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace aeon_sr {
namespace i18n {
namespace {

const imgui_function_table *g_reshade = nullptr;
imgui_function_table g_shaped{};

std::string vformat(const char *fmt, va_list args)
{
	va_list copy;
	va_copy(copy, args);
	const int n = std::vsnprintf(nullptr, 0, fmt, copy);
	va_end(copy);
	if (n <= 0)
		return std::string();
	std::string s(static_cast<size_t>(n) + 1, '\0');
	std::vsnprintf(s.data(), s.size(), fmt, args);
	s.resize(static_cast<size_t>(n));
	return s;
}

std::string shown(const char *text, const char *end)
{
	return bidi::visual(end != nullptr ? std::string_view(text, static_cast<size_t>(end - text)) : std::string_view(text));
}

void text_unformatted(const char *text, const char *end)
{
	if (text == nullptr || !bidi::needs_shaping(end != nullptr ? std::string_view(text, static_cast<size_t>(end - text)) : std::string_view(text))) {
		g_reshade->TextUnformatted(text, end);
		return;
	}
	const std::string s = shown(text, end);
	g_reshade->TextUnformatted(s.c_str(), s.c_str() + s.size());
}

void text_v(const char *fmt, va_list args)
{
	const std::string s = bidi::visual(vformat(fmt, args));
	g_reshade->TextUnformatted(s.c_str(), s.c_str() + s.size());
}

void call_colored(const ImVec4 &col, const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	g_reshade->TextColoredV(col, fmt, a);
	va_end(a);
}

void text_colored_v(const ImVec4 &col, const char *fmt, va_list args)
{
	call_colored(col, "%s", bidi::visual(vformat(fmt, args)).c_str());
}

void call_disabled(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	g_reshade->TextDisabledV(fmt, a);
	va_end(a);
}

void text_disabled_v(const char *fmt, va_list args)
{
	call_disabled("%s", bidi::visual(vformat(fmt, args)).c_str());
}

void call_wrapped(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	g_reshade->TextWrappedV(fmt, a);
	va_end(a);
}

void text_wrapped_v(const char *fmt, va_list args)
{
	call_wrapped("%s", bidi::visual(vformat(fmt, args)).c_str());
}

void call_tooltip(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	g_reshade->SetTooltipV(fmt, a);
	va_end(a);
}

void set_tooltip_v(const char *fmt, va_list args)
{
	call_tooltip("%s", bidi::visual(vformat(fmt, args)).c_str());
}

void call_item_tooltip(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	g_reshade->SetItemTooltipV(fmt, a);
	va_end(a);
}

void set_item_tooltip_v(const char *fmt, va_list args)
{
	call_item_tooltip("%s", bidi::visual(vformat(fmt, args)).c_str());
}

bool button(const char *label, const ImVec2 &size)
{
	return g_reshade->Button(bidi::visual(label).c_str(), size);
}

void progress_bar(float fraction, const ImVec2 &size, const char *overlay)
{
	if (overlay == nullptr) {
		g_reshade->ProgressBar(fraction, size, overlay);
		return;
	}
	g_reshade->ProgressBar(fraction, size, bidi::visual(overlay).c_str());
}

ImVec2 calc_text_size(const char *text, const char *end, bool hide_after_hash, float wrap_width)
{
	if (text == nullptr || !bidi::needs_shaping(end != nullptr ? std::string_view(text, static_cast<size_t>(end - text)) : std::string_view(text)))
		return g_reshade->CalcTextSize(text, end, hide_after_hash, wrap_width);
	const std::string s = shown(text, end);
	return g_reshade->CalcTextSize(s.c_str(), s.c_str() + s.size(), hide_after_hash, wrap_width);
}

void add_text(ImDrawList *dl, const ImVec2 &pos, ImU32 col, const char *text, const char *end)
{
	if (text == nullptr || !bidi::needs_shaping(end != nullptr ? std::string_view(text, static_cast<size_t>(end - text)) : std::string_view(text))) {
		g_reshade->ImDrawList_AddText(dl, pos, col, text, end);
		return;
	}
	const std::string s = shown(text, end);
	g_reshade->ImDrawList_AddText(dl, pos, col, s.c_str(), s.c_str() + s.size());
}

void add_text2(ImDrawList *dl, ImFont *font, float size, const ImVec2 &pos, ImU32 col, const char *text, const char *end,
	float wrap_width, const ImVec4 *clip)
{
	if (text == nullptr || !bidi::needs_shaping(end != nullptr ? std::string_view(text, static_cast<size_t>(end - text)) : std::string_view(text))) {
		g_reshade->ImDrawList_AddText2(dl, font, size, pos, col, text, end, wrap_width, clip);
		return;
	}
	const std::string s = shown(text, end);
	g_reshade->ImDrawList_AddText2(dl, font, size, pos, col, s.c_str(), s.c_str() + s.size(), wrap_width, clip);
}

}

void use_text_shaping(bool on)
{
	const imgui_function_table *&instance = imgui_function_table_instance();
	if (instance == nullptr)
		return;
	if (instance != &g_shaped && instance != g_reshade) {
		g_reshade = instance;
		g_shaped = *g_reshade;
		g_shaped.TextUnformatted = text_unformatted;
		g_shaped.TextV = text_v;
		g_shaped.TextColoredV = text_colored_v;
		g_shaped.TextDisabledV = text_disabled_v;
		g_shaped.TextWrappedV = text_wrapped_v;
		g_shaped.SetTooltipV = set_tooltip_v;
		g_shaped.SetItemTooltipV = set_item_tooltip_v;
		g_shaped.Button = button;
		g_shaped.ProgressBar = progress_bar;
		g_shaped.CalcTextSize = calc_text_size;
		g_shaped.ImDrawList_AddText = add_text;
		g_shaped.ImDrawList_AddText2 = add_text2;
	}
	instance = on ? &g_shaped : g_reshade;
}

}
}

#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"

#if AEONSR_PANEL_PREVIEW
#include <string>
#include <vector>
#endif

namespace aeon_sr {
namespace ui {

namespace icon {
constexpr const char *kTaa = "\xEF\x88\x99";
constexpr const char *kNeural = "\xEF\x83\x90";
constexpr const char *kExperiments = "\xEF\x83\x83";
constexpr const char *kDiagnostics = "\xEF\x88\x9E";
constexpr const char *kFrames = "\xEF\x80\x88";
constexpr const char *kOk = "\xEF\x81\x98";
constexpr const char *kFail = "\xEF\x81\x97";
constexpr const char *kWarn = "\xEF\x81\xB1";
constexpr const char *kOff = "\xEF\x81\x96";
constexpr const char *kUndo = "\xEF\x83\xA2";
constexpr const char *kRefresh = "\xEF\x80\xA1";
constexpr const char *kSave = "\xEF\x83\x87";
constexpr const char *kCopy = "\xEF\x83\x85";
constexpr const char *kCamera = "\xEF\x80\xB0";
constexpr const char *kPlay = "\xEF\x81\x8B";
constexpr const char *kStop = "\xEF\x81\x8D";
constexpr const char *kChevronDown = "\xEF\x81\xB8";
constexpr const char *kChevronRight = "\xEF\x81\x94";
constexpr const char *kInfo = "\xEF\x81\x9A";
constexpr const char *kAlert = "\xEF\x81\xAA";
constexpr const char *kTarget = "\xEF\x81\x9B";
}

struct Palette {
	ImVec4 accent, accent_end, accent_mark, accent_mark_end, knob, raised, loading;
	ImVec4 ok, warn, fail, off;
	ImVec4 text, muted, faded, label;
	ImVec4 dlss, fsr, xess, vsr;
	ImVec4 surface, header, surface_hi, border, tray, band;
};

const Palette &palette();

enum class Brand { Neutral, Nvidia, Amd, Intel, None };
const ImVec4 *brand_colour(Brand b);

ImVec4 mix(const ImVec4 &a, const ImVec4 &b, float t);
ImVec4 readable_on(const ImVec4 &bg);
float contrast(const ImVec4 &a, const ImVec4 &b);
ImVec4 with_alpha(ImVec4 c, float a);

class Theme {
public:
	Theme();
	~Theme();
	Theme(const Theme &) = delete;
	Theme &operator=(const Theme &) = delete;

private:
	int colours_ = 0;
	int vars_ = 0;
};

class Dim {
public:
	explicit Dim(bool on);
	~Dim();
	Dim(const Dim &) = delete;
	Dim &operator=(const Dim &) = delete;

private:
	bool on_;
};

void tooltip_alpha_begin();
void tooltip_alpha_end();

const char *glyph(const char *icon);

void banner(const char *title, const char *subtitle);

bool section(const char *id, const char *icon, const char *title, bool default_open,
	const ImVec4 *badge_colour = nullptr, const char *badge = nullptr,
	const char *tag = nullptr, Brand tag_brand = Brand::Neutral, bool lit = false, bool closed_only = false,
	const char *badge_short = nullptr);
void section_end();

void band_begin(const char *id);
void band_end(bool last = false);

void tray_begin(const char *id);
void tray_end();

bool disclosure(const char *id, const char *title);
void disclosure_end();

void subsection(const char *title, const char *aside = nullptr);

bool stacked_captions();
void need_stacked_captions();

float control_width();
void note_caption(float width);

bool toggle(const char *label, bool *value);
bool slider_float(const char *label, float *value, float min, float max, const char *format,
	ImGuiSliderFlags flags = 0);
bool slider_int(const char *label, int *value, int min, int max, const char *format);
bool button(const char *icon, const char *label);
float button_width(const char *icon, const char *label);
void same_line_if_fits(float w);

bool segment(const char *label, bool selected, float width, ImDrawFlags corners, bool divider,
	Brand brand = Brand::Neutral, bool lit = true, bool framed = true, bool halo = false);
void segment_frame(const ImVec2 &min, const ImVec2 &max, float line_h, int lines);
void segment_glow(const ImVec2 &min, const ImVec2 &max, const ImVec4 &colour);

void pill(const ImVec4 &colour, const char *text, bool wrap = true);
float pill_width(const char *text);

void brand_chip(Brand b, const char *text, bool lit);

void osd_begin(const char *id);
void osd_end();
void osd_mark();
void icon_text(const ImVec4 &colour, const char *glyph);
void callout(const ImVec4 &colour, const char *icon, const char *text);
void calloutf(const ImVec4 &colour, const char *icon, const char *fmt, ...) IM_FMTARGS(3);

struct Span {
	const char *text;
	ImVec4 colour;
};
void flow(const Span *spans, int count);
void wrapped(const ImVec4 &colour, const char *text);

float avail_width();

#if AEONSR_PANEL_PREVIEW
struct ContrastPair {
	std::string name;
	ImVec4 fg, bg;
	float min;
};
std::vector<ContrastPair> contrast_pairs();
#endif

}
}

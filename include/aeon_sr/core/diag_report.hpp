#pragma once

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/i18n.hpp"
#include "aeon_sr/core/panel.hpp"
#include "aeon_sr/core/settings.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aeon_sr {

enum class CheckState : unsigned int {
	Off,
	Ok,
	Warn,
	Fail,
};

const char *check_state_label(CheckState s) noexcept;

struct DiagCheck {
	const char *name = "";
	const char *title = "";
	CheckState state = CheckState::Off;
	std::string detail;
	std::string action;
	std::string tech;
};

std::vector<DiagCheck> diag_checks(const PanelState &state, const Settings &settings,
	i18n::Lang lang = i18n::Lang::en_US);

const char *diag_status_text(UpscalerStatus status, i18n::Lang lang) noexcept;

std::string diag_said(const std::string &english, i18n::Lang lang, i18n::S fallback, std::string *tech);

bool diag_is_technical(const std::string &s);

std::string diag_cards_text(const std::string &english_cards, i18n::Lang lang);

const char *diag_arch_text(uint32_t architecture, i18n::Lang lang);

CheckState diag_worst(const std::vector<DiagCheck> &checks) noexcept;

std::wstring diag_report_text(const PanelState &state, const Settings &settings);

std::wstring diag_write_report(const PanelState &state, const Settings &settings,
	std::wstring *error);

std::string diag_player_text(const std::string &raw, std::string *tech);

}

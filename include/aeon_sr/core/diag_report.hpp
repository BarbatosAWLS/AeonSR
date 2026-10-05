#pragma once

#include "aeon_sr/core/diagnostics.hpp"
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
	CheckState state = CheckState::Off;
	std::string detail;
	std::string action;
};

std::vector<DiagCheck> diag_checks(const PanelState &state, const Settings &settings);

CheckState diag_worst(const std::vector<DiagCheck> &checks) noexcept;

std::wstring diag_report_text(const PanelState &state, const Settings &settings);

std::wstring diag_write_report(const PanelState &state, const Settings &settings,
	std::wstring *error);

}

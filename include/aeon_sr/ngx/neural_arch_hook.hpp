#pragma once

#include <cstdint>

namespace aeon_sr {

struct NeuralArchHookStats {
	uint32_t query_resolved = 0;
	uint32_t direct_resolved = 0;
	uint32_t arch_asked = 0;
	uint32_t arch_substituted = 0;
	uint32_t last_true_architecture = 0;
};

bool neural_arch_hook_install(void *module) noexcept;

bool neural_arch_hook_remove(void *module) noexcept;

void neural_arch_hook_report(uint32_t architecture) noexcept;
uint32_t neural_arch_hook_reporting() noexcept;

NeuralArchHookStats neural_arch_hook_stats() noexcept;
void neural_arch_hook_reset_stats() noexcept;

}

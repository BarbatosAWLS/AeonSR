#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aeon_sr {

uint32_t neural_capability_for_architecture(uint32_t architecture) noexcept;

bool neural_kernel_runs_on(uint32_t sm_target, uint32_t capability) noexcept;

std::vector<uint32_t> neural_sm_targets_in(const char *bytes, size_t size,
	uint32_t *out_ptx_floor);

bool neural_runtime_serves(const std::vector<uint32_t> &targets, uint32_t ptx_floor,
	uint32_t architecture) noexcept;

const char *neural_sm_cards(uint32_t sm_target) noexcept;

const char *neural_arch_name(uint32_t id) noexcept;

struct NeuralKernelContainer {
	std::vector<uint32_t> cubins;
	uint32_t ptx_floor = 0u;
};

struct NeuralRuntimeKernels {
	std::vector<NeuralKernelContainer> containers;
	std::vector<uint32_t> text_targets;
	uint32_t text_ptx_floor = 0u;
};

NeuralRuntimeKernels neural_kernels_in(const char *bytes, size_t size);

bool neural_kernels_serve(const NeuralRuntimeKernels &kernels, uint32_t architecture) noexcept;

std::vector<uint32_t> neural_kernels_complete(const NeuralRuntimeKernels &kernels);
std::vector<uint32_t> neural_kernels_partial(const NeuralRuntimeKernels &kernels);

std::string neural_kernels_label(const NeuralRuntimeKernels &kernels);

std::string neural_kernels_cards(const NeuralRuntimeKernels &kernels);

struct NeuralRuntimeIdentity {
	std::string file_version;
	bool has_certificate = false;
};

NeuralRuntimeIdentity neural_identity_in(const char *bytes, size_t size);

uint32_t neural_constant_return(const unsigned char *code, size_t size) noexcept;

bool neural_allgpu_build_targets(uint32_t architecture) noexcept;

uint32_t neural_architecture_to_report(uint32_t card, uint32_t runtime_minimum,
	bool kernels_serve_card) noexcept;

std::vector<std::wstring> neural_runtime_candidates(const std::wstring &addon_dir,
	const std::wstring &exe_dir);

}

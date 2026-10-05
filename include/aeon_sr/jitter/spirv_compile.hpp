#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aeon_sr {

bool spirv_compile(const std::string &source, const std::string &entry_point,
	std::vector<uint8_t> &out_binary, std::string *out_entry_point, std::wstring *error);

inline constexpr uint32_t kSpirvMagic = 0x07230203u;

bool spirv_looks_like_module(const std::vector<uint8_t> &binary) noexcept;

enum class SpirvDescriptorKind : uint8_t { Other = 0, CombinedImageSampler, StorageImage, UniformBuffer };

struct SpirvBinding {
	uint32_t set = 0;
	uint32_t binding = 0;
	SpirvDescriptorKind kind = SpirvDescriptorKind::Other;

	bool operator==(const SpirvBinding &o) const noexcept
	{
		return set == o.set && binding == o.binding && kind == o.kind;
	}
};

std::vector<SpirvBinding> spirv_descriptor_bindings(const std::vector<uint8_t> &binary);

}

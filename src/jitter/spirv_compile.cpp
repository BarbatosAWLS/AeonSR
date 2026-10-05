#include "aeon_sr/jitter/spirv_compile.hpp"

#include "effect_codegen.hpp"
#include "effect_parser.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

namespace aeon_sr {
namespace {

std::wstring widen_ascii(const std::string &s)
{
	std::wstring w;
	w.reserve(s.size());
	for (const char c : s)
		w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
	return w;
}

}

bool spirv_looks_like_module(const std::vector<uint8_t> &binary) noexcept
{
	if (binary.size() < 4 || (binary.size() % 4) != 0)
		return false;
	uint32_t first = 0;
	std::memcpy(&first, binary.data(), sizeof(first));
	return first == kSpirvMagic || first == 0x03022307u;
}

std::vector<SpirvBinding> spirv_descriptor_bindings(const std::vector<uint8_t> &binary)
{
	constexpr uint32_t kOpTypeImage = 25, kOpTypeSampledImage = 27, kOpTypeStruct = 30,
		kOpTypePointer = 32, kOpVariable = 59, kOpDecorate = 71;
	constexpr uint32_t kDecorationBinding = 33, kDecorationDescriptorSet = 34;
	constexpr uint32_t kStorageClassUniform = 2;
	constexpr uint32_t kImageSampledForStorage = 2;

	std::vector<SpirvBinding> out;
	if (!spirv_looks_like_module(binary))
		return out;
	const size_t words = binary.size() / 4;
	const auto word = [&binary](size_t i) {
		uint32_t w = 0;
		std::memcpy(&w, binary.data() + i * 4, sizeof(w));
		return w;
	};
	if (words < 5 || word(0) != kSpirvMagic)
		return out;

	struct Decor { int64_t set = -1, binding = -1; };
	std::map<uint32_t, Decor> decorations;
	std::map<uint32_t, SpirvDescriptorKind> pointee_kind;
	std::map<uint32_t, uint32_t> struct_ids;
	struct Pointer { uint32_t storage_class = 0, type = 0; };
	std::map<uint32_t, Pointer> pointers;
	std::map<uint32_t, uint32_t> variables;

	for (size_t i = 5; i < words;) {
		const uint32_t w = word(i);
		const uint32_t count = w >> 16;
		const uint32_t op = w & 0xFFFFu;
		if (count == 0 || i + count > words)
			return {};
		if (op == kOpDecorate && count >= 4) {
			Decor &d = decorations[word(i + 1)];
			if (word(i + 2) == kDecorationDescriptorSet)
				d.set = word(i + 3);
			else if (word(i + 2) == kDecorationBinding)
				d.binding = word(i + 3);
		} else if (op == kOpTypeSampledImage && count >= 3) {
			pointee_kind[word(i + 1)] = SpirvDescriptorKind::CombinedImageSampler;
		} else if (op == kOpTypeImage && count >= 8) {
			if (word(i + 7) == kImageSampledForStorage)
				pointee_kind[word(i + 1)] = SpirvDescriptorKind::StorageImage;
		} else if (op == kOpTypeStruct && count >= 2) {
			struct_ids[word(i + 1)] = 1;
		} else if (op == kOpTypePointer && count >= 4) {
			pointers[word(i + 1)] = Pointer{ word(i + 2), word(i + 3) };
		} else if (op == kOpVariable && count >= 4) {
			variables[word(i + 2)] = word(i + 1);
		}
		i += count;
	}

	for (const auto &[id, d] : decorations) {
		if (d.set < 0 || d.binding < 0)
			continue;
		SpirvBinding b;
		b.set = static_cast<uint32_t>(d.set);
		b.binding = static_cast<uint32_t>(d.binding);
		const auto var = variables.find(id);
		if (var != variables.end()) {
			const auto ptr = pointers.find(var->second);
			if (ptr != pointers.end()) {
				const auto kind = pointee_kind.find(ptr->second.type);
				if (kind != pointee_kind.end())
					b.kind = kind->second;
				else if (ptr->second.storage_class == kStorageClassUniform &&
					struct_ids.count(ptr->second.type) != 0)
					b.kind = SpirvDescriptorKind::UniformBuffer;
			}
		}
		out.push_back(b);
	}
	std::sort(out.begin(), out.end(), [](const SpirvBinding &a, const SpirvBinding &b) {
		return a.set != b.set ? a.set < b.set : a.binding < b.binding;
	});
	return out;
}

bool spirv_compile(const std::string &source, const std::string &entry_point,
	std::vector<uint8_t> &out_binary, std::string *out_entry_point, std::wstring *error)
{
	out_binary.clear();
	if (out_entry_point != nullptr)
		out_entry_point->clear();
	if (error != nullptr)
		error->clear();

	const auto fail = [error](const char *what, const std::string &detail) {
		if (error != nullptr) {
			*error = widen_ascii(what);
			if (!detail.empty())
				*error += L": " + widen_ascii(detail);
		}
		return false;
	};

	if (source.empty())
		return fail("no shader source", {});
	if (entry_point.empty())
		return fail("no entry point named", {});

	const std::unique_ptr<reshadefx::codegen> backend(
		reshadefx::create_codegen_spirv(true, false, false));
	if (backend == nullptr)
		return fail("ReShade's SPIR-V code generator could not be created", {});

	reshadefx::parser parser;
	if (!parser.parse(source, backend.get()))
		return fail("the shader did not compile", parser.errors());

	const auto &points = backend->module().entry_points;
	std::string resolved;
	size_t matches = 0;
	for (const auto &ep : points) {
		if (ep.first == entry_point) {
			resolved = ep.first;
			matches = 1;
			break;
		}
		if (ep.first.find(entry_point) != std::string::npos) {
			resolved = ep.first;
			++matches;
		}
	}
	if (matches == 0 || matches > 1) {
		std::string have;
		for (const auto &ep : points)
			have += (have.empty() ? "" : ", ") + ep.first;
		if (have.empty())
			have = "nothing at all";
		return fail(matches > 1
			? "more than one entry point matches, and picking one would be a guess. The module has"
			: "the shader compiled but has no entry point matching that name. The module has", have);
	}

	std::string binary, assembly, errors;
	if (!backend->assemble_code_for_entry_point(resolved, binary, assembly, errors))
		return fail("the module could not be assembled", errors);

	out_binary.assign(binary.begin(), binary.end());
	if (!spirv_looks_like_module(out_binary)) {
		out_binary.clear();
		return fail("the assembler returned something that is not a SPIR-V module", {});
	}
	if (out_entry_point != nullptr)
		*out_entry_point = resolved;
	return true;
}

}

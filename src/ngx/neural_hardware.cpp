#include "aeon_sr/ngx/neural_hardware.hpp"
#include "aeon_sr/core/runtime_search.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace aeon_sr {

namespace {

uint16_t rd16(const unsigned char *p) noexcept
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rd32(const unsigned char *p) noexcept
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
		(static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t rd64(const unsigned char *p) noexcept
{
	return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32);
}

constexpr uint32_t kFatbinMagic = 0xBA55ED50u;
constexpr uint32_t kEntryPtx = 1u;
constexpr uint32_t kEntryCubin = 2u;

bool read_container(const unsigned char *b, size_t size, size_t off,
	NeuralKernelContainer *out, size_t *out_end) noexcept
{
	if (size - off < 16u)
		return false;
	const uint16_t version = rd16(b + off + 4);
	const uint16_t header = rd16(b + off + 6);
	const uint64_t body = rd64(b + off + 8);
	if (version != 1u || header < 16u || header > 64u || header > size - off)
		return false;
	if (body > size - off - header)
		return false;
	const size_t end = off + header + static_cast<size_t>(body);

	bool any = false;
	size_t p = off + header;
	while (end - p >= 16u) {
		const uint32_t kind = rd32(b + p);
		const uint32_t entry_header = rd32(b + p + 4);
		const uint64_t payload = rd64(b + p + 8);
		if (kind == 0u && entry_header == 0u)
			break;
		if (entry_header < 32u || entry_header > 4096u || entry_header > end - p ||
			payload > end - p - entry_header)
			return false;
		const uint32_t target = rd32(b + p + 28);
		if (target >= 10u && target <= 999u) {
			const uint32_t type = kind & 0xFFu;
			if (type == kEntryCubin) {
				auto &c = out->cubins;
				if (std::find(c.begin(), c.end(), target) == c.end()) {
					c.push_back(target);
					std::sort(c.begin(), c.end());
				}
				any = true;
			} else if (type == kEntryPtx) {
				if (out->ptx_floor == 0u || target < out->ptx_floor)
					out->ptx_floor = target;
				any = true;
			}
		}
		const size_t next = ((p - off) + entry_header + static_cast<size_t>(payload) + 7u) & ~size_t{ 7u };
		if (next > end - off)
			break;
		p = off + next;
	}
	*out_end = end;
	return any;
}

bool container_serves(const NeuralKernelContainer &c, uint32_t capability) noexcept
{
	for (const uint32_t t : c.cubins)
		if (neural_kernel_runs_on(t, capability))
			return true;
	return c.ptx_floor != 0u && c.ptx_floor <= capability;
}

}

uint32_t neural_capability_for_architecture(uint32_t architecture) noexcept
{
	switch (architecture) {
	case 0x140u: return 70u;
	case 0x150u: return 72u;
	case 0x160u: return 75u;
	case 0x170u: return 86u;
	case 0x180u: return 90u;
	case 0x190u: return 89u;
	case 0x1A0u: return 100u;
	case 0x1B0u: return 120u;
	default: return 0u;
	}
}

bool neural_kernel_runs_on(uint32_t sm_target, uint32_t capability) noexcept
{
	if (sm_target == 0u || capability == 0u)
		return false;
	if (sm_target / 10u != capability / 10u)
		return false;
	return sm_target <= capability;
}

std::vector<uint32_t> neural_sm_targets_in(const char *bytes, size_t size,
	uint32_t *out_ptx_floor)
{
	std::vector<uint32_t> out;
	if (out_ptx_floor != nullptr)
		*out_ptx_floor = 0u;
	if (bytes == nullptr)
		return out;

	static const char kSm[] = "-arch sm_";
	static const char kPtx[] = "-arch compute_";
	constexpr size_t kSmLen = sizeof(kSm) - 1u;
	constexpr size_t kPtxLen = sizeof(kPtx) - 1u;
	if (size <= kPtxLen + 4u)
		return out;

	const size_t guard = size - (kPtxLen + 4u);
	for (size_t i = 0; i < guard; ++i) {
		if (bytes[i] != '-')
			continue;
		if (memcmp(bytes + i, kPtx, kPtxLen) == 0) {
			uint32_t v = 0;
			size_t j = i + kPtxLen;
			while (j < size && bytes[j] >= '0' && bytes[j] <= '9' && v < 100000u)
				v = v * 10u + static_cast<uint32_t>(bytes[j++] - '0');
			if (v >= 10u && v <= 999u && out_ptx_floor != nullptr &&
				(*out_ptx_floor == 0u || v < *out_ptx_floor))
				*out_ptx_floor = v;
			continue;
		}
		if (memcmp(bytes + i, kSm, kSmLen) != 0)
			continue;
		uint32_t v = 0;
		size_t j = i + kSmLen;
		while (j < size && bytes[j] >= '0' && bytes[j] <= '9' && v < 100000u)
			v = v * 10u + static_cast<uint32_t>(bytes[j++] - '0');
		if (v < 10u || v > 999u)
			continue;
		if (std::find(out.begin(), out.end(), v) == out.end())
			out.push_back(v);
	}
	std::sort(out.begin(), out.end());
	return out;
}

bool neural_runtime_serves(const std::vector<uint32_t> &targets, uint32_t ptx_floor,
	uint32_t architecture) noexcept
{
	if (targets.empty() && ptx_floor == 0u)
		return true;
	const uint32_t capability = neural_capability_for_architecture(architecture);
	if (capability == 0u)
		return true;
	for (const uint32_t t : targets)
		if (neural_kernel_runs_on(t, capability))
			return true;
	return ptx_floor != 0u && ptx_floor <= capability;
}

const char *neural_arch_name(uint32_t id) noexcept
{
	switch (id) {
	case 0x0F0u: return "Kepler";
	case 0x110u: return "Maxwell";
	case 0x120u: return "Maxwell 2";
	case 0x130u: return "Pascal";
	case 0x140u: return "Volta";
	case 0x150u: return "Volta GV11x";
	case 0x160u: return "Turing, RTX 20";
	case 0x170u: return "Ampere, RTX 30";
	case 0x180u: return "Hopper";
	case 0x190u: return "Ada, RTX 40";
	case 0x1A0u: return "Blackwell GB10x";
	case 0x1B0u: return "Blackwell, RTX 50";
	default: return "an architecture this build has no name for";
	}
}

const char *neural_sm_cards(uint32_t sm_target) noexcept
{
	switch (sm_target) {
	case 70u: return "Volta";
	case 72u: return "Volta GV11x";
	case 75u: return "Turing, RTX 20";
	case 80u: return "Ampere GA100";
	case 86u: return "Ampere, RTX 30";
	case 89u: return "Ada, RTX 40";
	case 90u: return "Hopper";
	case 100u: return "Blackwell GB100";
	case 120u: return "Blackwell, RTX 50";
	default: return "cards this build has no name for";
	}
}

NeuralRuntimeKernels neural_kernels_in(const char *bytes, size_t size)
{
	NeuralRuntimeKernels out;
	out.text_targets = neural_sm_targets_in(bytes, size, &out.text_ptx_floor);
	if (bytes == nullptr || size < 16u)
		return out;
	const auto *const b = reinterpret_cast<const unsigned char *>(bytes);
	for (size_t i = 0; i + 16u <= size; ++i) {
		if (b[i] != 0x50u || rd32(b + i) != kFatbinMagic)
			continue;
		NeuralKernelContainer c;
		size_t end = 0;
		if (!read_container(b, size, i, &c, &end))
			continue;
		out.containers.push_back(std::move(c));
		i = end - 1u;
	}
	return out;
}

bool neural_kernels_serve(const NeuralRuntimeKernels &kernels, uint32_t architecture) noexcept
{
	if (kernels.containers.empty())
		return neural_runtime_serves(kernels.text_targets, kernels.text_ptx_floor, architecture);
	const uint32_t capability = neural_capability_for_architecture(architecture);
	if (capability == 0u)
		return true;
	for (const NeuralKernelContainer &c : kernels.containers)
		if (!container_serves(c, capability))
			return false;
	return true;
}

std::vector<uint32_t> neural_kernels_complete(const NeuralRuntimeKernels &kernels)
{
	if (kernels.containers.empty())
		return kernels.text_targets;
	std::vector<uint32_t> out = kernels.containers.front().cubins;
	for (const NeuralKernelContainer &c : kernels.containers) {
		std::vector<uint32_t> keep;
		std::set_intersection(out.begin(), out.end(), c.cubins.begin(), c.cubins.end(),
			std::back_inserter(keep));
		out.swap(keep);
	}
	return out;
}

std::vector<uint32_t> neural_kernels_partial(const NeuralRuntimeKernels &kernels)
{
	std::vector<uint32_t> all;
	for (const NeuralKernelContainer &c : kernels.containers)
		for (const uint32_t t : c.cubins)
			if (std::find(all.begin(), all.end(), t) == all.end())
				all.push_back(t);
	std::sort(all.begin(), all.end());
	const std::vector<uint32_t> complete = neural_kernels_complete(kernels);
	std::vector<uint32_t> out;
	std::set_difference(all.begin(), all.end(), complete.begin(), complete.end(),
		std::back_inserter(out));
	return out;
}

std::string neural_kernels_label(const NeuralRuntimeKernels &kernels)
{
	auto append = [](std::string *s, const std::vector<uint32_t> &targets) {
		for (const uint32_t t : targets) {
			char buf[64]{};
			snprintf(buf, sizeof buf, "%ssm_%u (%s)", s->empty() ? "" : ", ", t, neural_sm_cards(t));
			*s += buf;
		}
	};
	std::string out;
	append(&out, neural_kernels_complete(kernels));

	uint32_t ptx = kernels.text_ptx_floor;
	if (!kernels.containers.empty()) {
		ptx = 0u;
		for (const NeuralKernelContainer &c : kernels.containers) {
			if (c.ptx_floor == 0u) {
				ptx = 0u;
				break;
			}
			ptx = std::max(ptx, c.ptx_floor);
		}
	}
	if (ptx != 0u) {
		char buf[64]{};
		snprintf(buf, sizeof buf, "%splus PTX from compute_%u up", out.empty() ? "" : ", ", ptx);
		out += buf;
	}

	const std::vector<uint32_t> partial = neural_kernels_partial(kernels);
	if (!partial.empty()) {
		std::string some;
		append(&some, partial);
		out += (out.empty() ? "" : "; ") + std::string("in some containers only: ") + some;
	}
	return out;
}

std::string neural_kernels_cards(const NeuralRuntimeKernels &kernels)
{
	if (kernels.containers.empty() && kernels.text_targets.empty() && kernels.text_ptx_floor == 0u)
		return {};
	static constexpr struct { uint32_t arch; const char *name; } kGenerations[] = {
		{ 0x160u, "20" }, { 0x170u, "30" }, { 0x190u, "40" }, { 0x1B0u, "50" },
	};
	std::vector<const char *> served;
	for (const auto &g : kGenerations)
		if (neural_kernels_serve(kernels, g.arch))
			served.push_back(g.name);
	if (served.empty())
		return "no RTX 20, 30, 40 or 50 card";
	std::string out = "RTX ";
	for (size_t i = 0; i < served.size(); ++i) {
		if (i > 0)
			out += i + 1 == served.size() ? " and " : ", ";
		out += served[i];
	}
	return out;
}

NeuralRuntimeIdentity neural_identity_in(const char *bytes, size_t size)
{
	NeuralRuntimeIdentity out;
	if (bytes == nullptr || size < 0x40u)
		return out;
	const auto *const b = reinterpret_cast<const unsigned char *>(bytes);

	if (b[0] == 'M' && b[1] == 'Z') {
		const uint32_t pe = rd32(b + 0x3C);
		if (pe <= size - 24u && rd32(b + pe) == 0x00004550u) {
			const size_t opt = pe + 24u;
			const uint16_t magic = size - opt >= 2u ? rd16(b + opt) : 0u;
			const size_t dirs = magic == 0x20Bu ? opt + 112u : magic == 0x10Bu ? opt + 96u : 0u;
			if (dirs != 0u && dirs <= size - 40u && rd32(b + dirs - 4u) > 4u) {
				const uint32_t at = rd32(b + dirs + 32u);
				const uint32_t bytes_in = rd32(b + dirs + 36u);
				out.has_certificate = bytes_in >= 8u && at >= dirs && at <= size &&
					bytes_in <= size - at;
			}
		}
	}

	static const wchar_t kKey[] = L"FileVersion";
	constexpr size_t kKeyBytes = sizeof(kKey);
	for (size_t i = 6u; i + kKeyBytes + 64u <= size; ++i) {
		if (b[i] != 'F' || b[i + 1] != 0u || memcmp(b + i, kKey, kKeyBytes) != 0)
			continue;
		const size_t start = i - 6u;
		if (rd16(b + start + 4) != 1u)
			continue;
		const size_t chars = rd16(b + start + 2);
		size_t v = start + ((6u + kKeyBytes + 3u) & ~size_t{ 3u });
		std::string value;
		bool clean = chars >= 2u && chars <= 32u;
		for (size_t k = 0; clean && k < chars && v + 1u < size; ++k, v += 2u) {
			const uint16_t ch = rd16(b + v);
			if (ch == 0u)
				break;
			if (ch < 0x20u || ch > 0x7Eu)
				clean = false;
			else
				value.push_back(static_cast<char>(ch));
		}
		if (clean && !value.empty()) {
			out.file_version = value;
			break;
		}
	}
	return out;
}

uint32_t neural_constant_return(const unsigned char *code, size_t size) noexcept
{
	if (code == nullptr || size < 6u || code[0] != 0xB8u || code[5] != 0xC3u)
		return 0u;
	return rd32(code + 1);
}

bool neural_allgpu_build_targets(uint32_t architecture) noexcept
{
	return architecture == 0x160u || architecture == 0x170u || architecture == 0x190u;
}

uint32_t neural_architecture_to_report(uint32_t card, uint32_t runtime_minimum,
	bool kernels_serve_card) noexcept
{
	if (!kernels_serve_card || card == 0u || runtime_minimum == 0u)
		return 0u;
	if (neural_capability_for_architecture(card) == 0u ||
		neural_capability_for_architecture(runtime_minimum) == 0u)
		return 0u;
	return card < runtime_minimum ? runtime_minimum : 0u;
}

std::vector<std::wstring> neural_runtime_candidates(const std::wstring &addon_dir,
	const std::wstring &exe_dir)
{
	std::vector<std::wstring> out;
	auto listed = [&out](const std::wstring &path) {
		for (const std::wstring &o : out)
			if (_wcsicmp(o.c_str(), path.c_str()) == 0)
				return true;
		return false;
	};
	for (const std::wstring &dir : preferred_search_dirs(addon_dir, exe_dir, kNeuralPreferredSubdir)) {
		const std::wstring where = path_under(dir, L"");
		std::vector<std::wstring> names;
		WIN32_FIND_DATAW fd{};
		const HANDLE h = FindFirstFileW((where + L"nvngx_dlssnr*.dll").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
					continue;
				const size_t n = wcslen(fd.cFileName);
				if (n < 4u || _wcsicmp(fd.cFileName + n - 4u, L".dll") != 0)
					continue;
				names.emplace_back(fd.cFileName);
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		std::sort(names.begin(), names.end(), [](const std::wstring &a, const std::wstring &c) {
			const bool a_canonical = _wcsicmp(a.c_str(), L"nvngx_dlssnr.dll") == 0;
			const bool c_canonical = _wcsicmp(c.c_str(), L"nvngx_dlssnr.dll") == 0;
			if (a_canonical != c_canonical)
				return a_canonical;
			return _wcsicmp(a.c_str(), c.c_str()) < 0;
		});
		for (const std::wstring &n : names) {
			const std::wstring path = where + n;
			if (!listed(path))
				out.push_back(path);
		}
	}
	return out;
}

}

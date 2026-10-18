#pragma once

#include <d3d12.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace aeon_sr {

inline constexpr uint32_t kVendorMaxGenerated = 5;

enum class FrameGenerator : uint8_t {
	AeonSR = 0,
	Fsr = 1,
	Xess = 2,
	Dlss = 3,
};
inline constexpr uint32_t kFrameGeneratorCount = 4;

inline const char *frame_generator_label(FrameGenerator g) noexcept
{
	switch (g) {
	case FrameGenerator::Fsr: return "AMD FSR";
	case FrameGenerator::Xess: return "Intel XeSS";
	case FrameGenerator::Dlss: return "NVIDIA DLSS";
	default: return "AeonSR";
	}
}

inline const char *frame_generator_key(FrameGenerator g) noexcept
{
	switch (g) {
	case FrameGenerator::Fsr: return "FSR";
	case FrameGenerator::Xess: return "XeSS";
	case FrameGenerator::Dlss: return "DLSS";
	default: return "AeonSR";
	}
}

inline bool parse_frame_generator(std::string_view text, FrameGenerator *out) noexcept
{
	const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
	while (!text.empty() && blank(text.front()))
		text.remove_prefix(1);
	while (!text.empty() && blank(text.back()))
		text.remove_suffix(1);
	if (text.size() == 1 && text[0] >= '0' && text[0] < static_cast<char>('0' + kFrameGeneratorCount)) {
		*out = static_cast<FrameGenerator>(text[0] - '0');
		return true;
	}
	for (uint32_t i = 0; i < kFrameGeneratorCount; ++i) {
		const std::string_view key = frame_generator_key(static_cast<FrameGenerator>(i));
		bool same = key.size() == text.size();
		for (size_t c = 0; c < key.size() && same; ++c)
			same = lower(key[c]) == lower(text[c]);
		if (same) {
			*out = static_cast<FrameGenerator>(i);
			return true;
		}
	}
	return false;
}

enum class VendorColor : uint8_t {
	Sdr = 0,
	Pq = 1,
	Scrgb = 2,
};

DXGI_FORMAT vendor_frame_format(DXGI_FORMAT f) noexcept;

struct VendorFrame {
	ID3D12Device *device = nullptr;
	ID3D12CommandQueue *queue = nullptr;
	ID3D12GraphicsCommandList *cmd = nullptr;
	std::function<bool()> flush;

	ID3D12Resource *color = nullptr;
	ID3D12Resource *motion = nullptr;
	D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_COMMON;
	ID3D12Resource *depth = nullptr;
	ID3D12Resource *output = nullptr;
	uint32_t generated = 1;
	ID3D12Resource *more_outputs[kVendorMaxGenerated - 1] = {};

	uint32_t width = 0, height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	VendorColor color_space = VendorColor::Sdr;
	bool reset = false;
	double frame_ms = 16.7;
	HWND window = nullptr;
};

enum class VendorStep : uint8_t {
	Generated = 0,
	Warming,
	Building,
	Unavailable,
	Failed,
	ListLost,
};

class VendorFrameGenerator {
public:
	static constexpr D3D12_RESOURCE_STATES kInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	static constexpr D3D12_RESOURCE_STATES kOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	virtual ~VendorFrameGenerator() = default;
	virtual FrameGenerator kind() const noexcept = 0;
	virtual VendorStep record(const VendorFrame &f, std::wstring *note) = 0;
	virtual void reset() noexcept = 0;
	virtual std::wstring version() const = 0;
	virtual uint32_t max_generated() const noexcept { return 1; }
	virtual bool busy() const noexcept { return false; }
};

std::unique_ptr<VendorFrameGenerator> make_vendor_frame_generator(FrameGenerator g);
std::unique_ptr<VendorFrameGenerator> make_inprocess_vendor_frame_generator(FrameGenerator g);

}

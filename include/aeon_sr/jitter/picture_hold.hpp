#pragma once

#include <cstdint>

namespace aeon_sr {

class PictureHold {
public:
	static constexpr uint32_t kMinEdges = 256u;
	static constexpr float kHeldShare = 0.02f;
	static constexpr uint32_t kConfirm = 2u;

	enum class Change : uint8_t { None, Holds, Moves };

	Change feed(bool offset_moved, uint32_t edges, uint32_t changed) noexcept
	{
		if (edges < kMinEdges)
			return Change::None;
		const bool still = static_cast<float>(changed) <= kHeldShare * static_cast<float>(edges);
		if (!still) {
			streak_ = 0;
			if (!held_)
				return Change::None;
			held_ = false;
			return Change::Moves;
		}
		if (!offset_moved || held_)
			return Change::None;
		if (++streak_ < kConfirm)
			return Change::None;
		held_ = true;
		return Change::Holds;
	}
	bool held() const noexcept { return held_; }
	void reset() noexcept { *this = PictureHold{}; }

private:
	uint32_t streak_ = 0;
	bool held_ = false;
};

}

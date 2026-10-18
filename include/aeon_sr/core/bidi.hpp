#pragma once

#include <string>
#include <string_view>

namespace aeon_sr {
namespace bidi {

bool needs_shaping(std::string_view text) noexcept;

int direction(std::string_view text) noexcept;

std::string visual(std::string_view text, int paragraph = 0);

}
}

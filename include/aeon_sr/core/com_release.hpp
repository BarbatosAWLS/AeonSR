#pragma once

namespace aeon_sr {

template <typename T>
inline void safe_release(T *&p) noexcept
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

}

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace aeon_sr {

class RetryBackoff {
public:
	static constexpr uint64_t kFirstDelayMs = 2000;
	static constexpr uint64_t kMaxDelayMs = 60000;
	static constexpr size_t kKeys = 8;

	bool may_try(uint64_t now_ms, uint64_t key) const noexcept
	{
		const Entry *const e = find(key);
		return e == nullptr || now_ms >= e->next_ms;
	}

	void failed(uint64_t now_ms, uint64_t key, std::wstring why = {})
	{
		Entry *e = find(key);
		if (e != nullptr) {
			e->delay_ms = std::min(e->delay_ms * 2, kMaxDelayMs);
		} else {
			e = slot_for_new();
			*e = Entry{};
			e->used = true;
			e->key = key;
			e->delay_ms = kFirstDelayMs;
		}
		e->next_ms = now_ms + e->delay_ms;
		e->why = std::move(why);
	}

	void succeeded(uint64_t key) noexcept
	{
		if (Entry *const e = find(key))
			*e = Entry{};
	}

	bool failing(uint64_t key) const noexcept { return find(key) != nullptr; }

	uint64_t delay_ms(uint64_t key) const noexcept
	{
		const Entry *const e = find(key);
		return e != nullptr ? e->delay_ms : 0;
	}

	const std::wstring &reason(uint64_t key) const noexcept
	{
		static const std::wstring none;
		const Entry *const e = find(key);
		return e != nullptr ? e->why : none;
	}

private:
	struct Entry {
		bool used = false;
		uint64_t key = 0;
		uint64_t next_ms = 0;
		uint64_t delay_ms = 0;
		std::wstring why;
	};

	const Entry *find(uint64_t key) const noexcept
	{
		for (const Entry &e : entries_)
			if (e.used && e.key == key)
				return &e;
		return nullptr;
	}

	Entry *find(uint64_t key) noexcept
	{
		return const_cast<Entry *>(static_cast<const RetryBackoff *>(this)->find(key));
	}

	Entry *slot_for_new() noexcept
	{
		Entry *soonest = &entries_[0];
		for (Entry &e : entries_) {
			if (!e.used)
				return &e;
			if (e.next_ms < soonest->next_ms)
				soonest = &e;
		}
		return soonest;
	}

	Entry entries_[kKeys]{};
};

}

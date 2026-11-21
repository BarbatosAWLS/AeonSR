#pragma once

#include <d3dcompiler.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

namespace aeon_sr {

namespace shader_cache_detail {

struct Slot {
	std::mutex lock;
	ID3DBlob *blob = nullptr;
};

struct Table {
	std::mutex lock;
	std::map<std::tuple<const void *, std::string, std::string, unsigned>, std::unique_ptr<Slot>> slots;
};

inline Table &table()
{
	static Table *const t = new Table();
	return *t;
}

}

inline ID3DBlob *shader_cache_compile(const char *source, size_t length, const char *name, const char *entry,
	const char *target, unsigned flags, ID3DBlob **errors = nullptr)
{
	using namespace shader_cache_detail;
	if (errors != nullptr)
		*errors = nullptr;
	Slot *slot = nullptr;
	{
		Table &t = table();
		std::lock_guard<std::mutex> lock(t.lock);
		std::unique_ptr<Slot> &s = t.slots[std::make_tuple(static_cast<const void *>(source), std::string(entry),
			std::string(target), flags)];
		if (!s)
			s = std::make_unique<Slot>();
		slot = s.get();
	}
	std::lock_guard<std::mutex> lock(slot->lock);
	if (slot->blob == nullptr) {
		ID3DBlob *made = nullptr, *err = nullptr;
		if (FAILED(D3DCompile(source, length, name, nullptr, nullptr, entry, target, flags, 0, &made, &err)) ||
			made == nullptr) {
			if (made != nullptr)
				made->Release();
			if (errors != nullptr)
				*errors = err;
			else if (err != nullptr)
				err->Release();
			return nullptr;
		}
		if (err != nullptr)
			err->Release();
		slot->blob = made;
	}
	slot->blob->AddRef();
	return slot->blob;
}

inline void shader_cache_warm(const char *source, size_t length, const char *name, const char *entry,
	const char *target, unsigned flags)
{
	if (ID3DBlob *const b = shader_cache_compile(source, length, name, entry, target, flags))
		b->Release();
}

}

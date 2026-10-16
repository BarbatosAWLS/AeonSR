#include "aeon_sr/games/signature_scan.hpp"

#include <cstring>

namespace aeon_sr::games {

namespace {

bool readable(const uint8_t *p, size_t len)
{
	const uint8_t *const end = p + len;
	while (p < end) {
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
			(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 || mbi.Protect == 0)
			return false;
		p = static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
	}
	return true;
}

int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

}

bool parse_pattern(const char *text, BytePattern *out)
{
	out->bytes.clear();
	out->known.clear();
	if (text == nullptr)
		return false;
	for (const char *p = text; *p != '\0';) {
		if (*p == ' ') {
			++p;
			continue;
		}
		if (*p == '?') {
			++p;
			if (*p == '?')
				++p;
			if (*p != '\0' && *p != ' ')
				return false;
			out->bytes.push_back(0);
			out->known.push_back(0);
			continue;
		}
		const int hi = hex_digit(p[0]);
		const int lo = hi >= 0 ? hex_digit(p[1]) : -1;
		if (hi < 0 || lo < 0 || (p[2] != '\0' && p[2] != ' '))
			return false;
		out->bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
		out->known.push_back(1);
		p += 2;
	}
	return !out->bytes.empty() && out->known[0] != 0;
}

size_t find_pattern(const uint8_t *data, size_t size, const BytePattern &p, size_t *offsets, size_t limit)
{
	const size_t n = p.bytes.size();
	if (data == nullptr || n == 0 || size < n)
		return 0;
	size_t count = 0;
	const uint8_t first = p.bytes[0];
	for (size_t i = 0; i + n <= size;) {
		const void *hit = std::memchr(data + i, first, size - n - i + 1);
		if (hit == nullptr)
			break;
		i = static_cast<size_t>(static_cast<const uint8_t *>(hit) - data);
		bool match = true;
		for (size_t k = 1; k < n; ++k) {
			if (p.known[k] && data[i + k] != p.bytes[k]) {
				match = false;
				break;
			}
		}
		if (match) {
			if (count < limit && offsets != nullptr)
				offsets[count] = i;
			++count;
		}
		++i;
	}
	return count;
}

bool module_image(const void *base, ModuleImage *out)
{
	*out = ModuleImage{};
	if (base == nullptr)
		return false;
	const auto *b = reinterpret_cast<const uint8_t *>(reinterpret_cast<uintptr_t>(base) & ~uintptr_t(3));
	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(b);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000)
		return false;
	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(b + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;
	out->base = b;
	out->timestamp = nt->FileHeader.TimeDateStamp;
	out->size_of_image = nt->OptionalHeader.SizeOfImage;
	out->size = out->size_of_image;
	const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
		if ((s->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
			continue;
		const size_t start = s->VirtualAddress;
		size_t len = s->Misc.VirtualSize != 0 ? s->Misc.VirtualSize : s->SizeOfRawData;
		if (start >= out->size)
			continue;
		if (len > out->size - start)
			len = out->size - start;
		if (!readable(b + start, len))
			continue;
		out->code.emplace_back(start, len);
	}
	return !out->code.empty();
}

size_t find_in_code(const ModuleImage &image, const BytePattern &p, const uint8_t **found, size_t limit)
{
	size_t count = 0;
	std::vector<size_t> offs(limit != 0 ? limit : 1);
	for (const auto &sec : image.code) {
		const size_t got = find_pattern(image.base + sec.first, sec.second, p, offs.data(), limit);
		for (size_t i = 0; i < got && i < limit; ++i) {
			if (count + i < limit && found != nullptr)
				found[count + i] = image.base + sec.first + offs[i];
		}
		count += got;
	}
	return count;
}

}

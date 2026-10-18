#include "aeon_sr/core/report_bundle.hpp"
#include "aeon_sr/core/com_release.hpp"

#include "aeon_sr/core/half_float.hpp"

#include <Windows.h>
#include <dxgiformat.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aeon_sr {
namespace {

void put16(std::vector<uint8_t> &o, uint32_t v)
{
	o.push_back(static_cast<uint8_t>(v));
	o.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t> &o, uint32_t v)
{
	put16(o, v & 0xFFFFu);
	put16(o, v >> 16);
}

uint32_t get16(const uint8_t *p) noexcept { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8); }
uint32_t get32(const uint8_t *p) noexcept { return get16(p) | (get16(p + 2) << 16); }

struct ComScope {
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	~ComScope()
	{
		if (SUCCEEDED(hr))
			CoUninitialize();
	}
};

std::wstring hr_text(const wchar_t *what, HRESULT hr)
{
	wchar_t b[128]{};
	_snwprintf_s(b, _TRUNCATE, L"%ls failed (0x%08lX)", what, static_cast<unsigned long>(hr));
	return b;
}

float srgb_encode(float linear) noexcept
{
	const float c = linear <= 0.0f ? 0.0f : linear >= 1.0f ? 1.0f : linear;
	return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

bool read_all(const std::wstring &path, std::vector<uint8_t> *out)
{
	std::string s;
	bool cut = false;
	if (!read_shared_tail(path, ~0ull, &s, &cut))
		return false;
	out->assign(s.begin(), s.end());
	return true;
}

std::string narrow(const std::wstring &w)
{
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? static_cast<size_t>(n) : 0u, '\0');
	if (n > 0)
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

}

uint32_t crc32_of(const void *data, size_t size, uint32_t crc) noexcept
{
	static const auto table = [] {
		struct T {
			uint32_t v[256];
		} t{};
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			t.v[i] = c;
		}
		return t;
	}();
	const auto *p = static_cast<const uint8_t *>(data);
	crc = ~crc;
	for (size_t i = 0; i < size; ++i)
		crc = table.v[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
	return ~crc;
}

bool ZipBuilder::add(const std::string &name, const void *data, size_t size, uint16_t dos_time, uint16_t dos_date)
{
	if (finished_ || name.empty() || name.size() > 0xFFFFu)
		return false;
	const uint64_t after = static_cast<uint64_t>(out_.size()) + 30u + name.size() + size;
	uint64_t directory = 22u;
	for (const Entry &e : entries_)
		directory += 46u + e.name.size();
	directory += 46u + name.size();
	if (after + directory > 0xFFFFFFFFull || entries_.size() >= 0xFFFFu)
		return false;
	Entry e;
	e.name = name;
	e.crc = crc32_of(data, size);
	e.size = static_cast<uint32_t>(size);
	e.offset = static_cast<uint32_t>(out_.size());
	e.time = dos_time;
	e.date = dos_date;
	put32(out_, 0x04034B50u);
	put16(out_, 10u);
	put16(out_, 0x0800u);
	put16(out_, 0u);
	put16(out_, e.time);
	put16(out_, e.date);
	put32(out_, e.crc);
	put32(out_, e.size);
	put32(out_, e.size);
	put16(out_, static_cast<uint32_t>(name.size()));
	put16(out_, 0u);
	out_.insert(out_.end(), name.begin(), name.end());
	const auto *p = static_cast<const uint8_t *>(data);
	if (size != 0)
		out_.insert(out_.end(), p, p + size);
	entries_.push_back(std::move(e));
	return true;
}

void ZipBuilder::finish()
{
	if (finished_)
		return;
	finished_ = true;
	const uint32_t start = static_cast<uint32_t>(out_.size());
	for (const Entry &e : entries_) {
		put32(out_, 0x02014B50u);
		put16(out_, 20u);
		put16(out_, 10u);
		put16(out_, 0x0800u);
		put16(out_, 0u);
		put16(out_, e.time);
		put16(out_, e.date);
		put32(out_, e.crc);
		put32(out_, e.size);
		put32(out_, e.size);
		put16(out_, static_cast<uint32_t>(e.name.size()));
		put16(out_, 0u);
		put16(out_, 0u);
		put16(out_, 0u);
		put16(out_, 0u);
		put32(out_, 0u);
		put32(out_, e.offset);
		out_.insert(out_.end(), e.name.begin(), e.name.end());
	}
	const uint32_t size = static_cast<uint32_t>(out_.size()) - start;
	put32(out_, 0x06054B50u);
	put16(out_, 0u);
	put16(out_, 0u);
	put16(out_, static_cast<uint32_t>(entries_.size()));
	put16(out_, static_cast<uint32_t>(entries_.size()));
	put32(out_, size);
	put32(out_, start);
	put16(out_, 0u);
}

bool read_stored_zip(const std::vector<uint8_t> &zip, std::vector<ZipEntryRead> *out)
{
	out->clear();
	if (zip.size() < 22u)
		return false;
	size_t end = zip.size() - 22u;
	for (;; --end) {
		if (get32(&zip[end]) == 0x06054B50u)
			break;
		if (end == 0 || zip.size() - end > 22u + 0xFFFFu)
			return false;
	}
	const uint32_t count = get16(&zip[end + 10]);
	const uint32_t cd_size = get32(&zip[end + 12]);
	const uint32_t cd_at = get32(&zip[end + 16]);
	if (static_cast<uint64_t>(cd_at) + cd_size > end)
		return false;
	size_t at = cd_at;
	for (uint32_t i = 0; i < count; ++i) {
		if (at + 46u > end || get32(&zip[at]) != 0x02014B50u)
			return false;
		const uint32_t method = get16(&zip[at + 10]);
		const uint32_t crc = get32(&zip[at + 16]);
		const uint32_t csize = get32(&zip[at + 20]);
		const uint32_t usize = get32(&zip[at + 24]);
		const uint32_t nlen = get16(&zip[at + 28]), xlen = get16(&zip[at + 30]), clen = get16(&zip[at + 32]);
		const uint32_t local = get32(&zip[at + 42]);
		if (method != 0u || csize != usize || at + 46u + nlen > end)
			return false;
		ZipEntryRead e;
		e.name.assign(reinterpret_cast<const char *>(&zip[at + 46]), nlen);
		if (static_cast<uint64_t>(local) + 30u > zip.size() || get32(&zip[local]) != 0x04034B50u)
			return false;
		const size_t data = static_cast<size_t>(local) + 30u + get16(&zip[local + 26]) + get16(&zip[local + 28]);
		if (data + usize > zip.size())
			return false;
		e.data.assign(zip.begin() + static_cast<ptrdiff_t>(data), zip.begin() + static_cast<ptrdiff_t>(data + usize));
		e.crc_ok = crc32_of(e.data.data(), e.data.size()) == crc;
		out->push_back(std::move(e));
		at += 46u + nlen + xlen + clen;
	}
	return true;
}

bool encode_png(const uint8_t *rows, uint32_t width, uint32_t height, uint32_t row_bytes, uint32_t dxgi_format,
	std::vector<uint8_t> *png, bool *lossless, std::wstring *error)
{
	png->clear();
	*lossless = false;
	if (rows == nullptr || width == 0 || height == 0) {
		*error = L"the plane is empty";
		return false;
	}
	const DXGI_FORMAT f = static_cast<DXGI_FORMAT>(dxgi_format);
	const bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
		f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_TYPELESS ||
		f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
	const bool rgba = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
		f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	const bool ten = f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS;
	const bool half = f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS;
	if (!bgra && !rgba && !ten && !half) {
		wchar_t b[96]{};
		_snwprintf_s(b, _TRUNCATE, L"no PNG for DXGI format %u", dxgi_format);
		*error = b;
		return false;
	}
	const uint32_t in_bytes = half ? 8u : 4u;
	if (row_bytes < width * in_bytes) {
		*error = L"the plane's rows are shorter than its width";
		return false;
	}
	const bool deep = ten || half;
	const uint32_t stride = width * (deep ? 6u : 3u);
	std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height);
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *src = rows + static_cast<size_t>(y) * row_bytes;
		uint8_t *dst = pixels.data() + static_cast<size_t>(y) * stride;
		for (uint32_t x = 0; x < width; ++x) {
			if (bgra || rgba) {
				const uint8_t *p = src + x * 4u;
				dst[x * 3u + 0] = bgra ? p[0] : p[2];
				dst[x * 3u + 1] = p[1];
				dst[x * 3u + 2] = bgra ? p[2] : p[0];
				continue;
			}
			uint16_t c[3];
			if (ten) {
				uint32_t v;
				std::memcpy(&v, src + x * 4u, 4);
				for (int k = 0; k < 3; ++k) {
					const uint32_t ch = (v >> (10 * k)) & 0x3FFu;
					c[k] = static_cast<uint16_t>((ch << 6) | (ch >> 4));
				}
			} else {
				for (int k = 0; k < 3; ++k) {
					uint16_t h;
					std::memcpy(&h, src + x * 8u + k * 2u, 2);
					c[k] = static_cast<uint16_t>(std::lround(srgb_encode(half_to_float(h)) * 65535.0f));
				}
			}
			std::memcpy(dst + x * 6u, c, 6);
		}
	}

	const ComScope com;
	IWICImagingFactory *factory = nullptr;
	IStream *stream = nullptr;
	IWICBitmapEncoder *encoder = nullptr;
	IWICBitmapFrameEncode *frame = nullptr;
	IPropertyBag2 *props = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	bool ok = false;
	if (FAILED(hr)) {
		*error = hr_text(L"the Windows imaging component", hr);
	} else if (FAILED(hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
		*error = hr_text(L"a memory stream", hr);
	} else if (FAILED(hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
		FAILED(hr = encoder->Initialize(stream, WICBitmapEncoderNoCache)) ||
		FAILED(hr = encoder->CreateNewFrame(&frame, &props)) || FAILED(hr = frame->Initialize(props)) ||
		FAILED(hr = frame->SetSize(width, height))) {
		*error = hr_text(L"the PNG encoder", hr);
	} else {
		WICPixelFormatGUID want = deep ? GUID_WICPixelFormat48bppRGB : GUID_WICPixelFormat24bppBGR;
		const WICPixelFormatGUID asked = want;
		if (FAILED(hr = frame->SetPixelFormat(&want)) || want != asked) {
			*error = FAILED(hr) ? hr_text(L"the PNG pixel format", hr) : L"the PNG encoder changed the pixel format";
		} else if (FAILED(hr = frame->WritePixels(height, stride, static_cast<UINT>(pixels.size()), pixels.data())) ||
			FAILED(hr = frame->Commit()) || FAILED(hr = encoder->Commit())) {
			*error = hr_text(L"writing the PNG", hr);
		} else {
			STATSTG st{};
			LARGE_INTEGER zero{};
			if (SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME)) && SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
				png->resize(static_cast<size_t>(st.cbSize.QuadPart));
				ULONG got = 0;
				ok = SUCCEEDED(stream->Read(png->data(), static_cast<ULONG>(png->size()), &got)) && got == png->size();
			}
			if (!ok)
				*error = L"the PNG could not be read back from memory";
		}
	}
	safe_release(props);
	safe_release(frame);
	safe_release(encoder);
	safe_release(stream);
	safe_release(factory);
	*lossless = ok && !half;
	return ok;
}

bool decode_png(const std::vector<uint8_t> &png, uint32_t *width, uint32_t *height, uint32_t *bits,
	std::vector<uint8_t> *pixels)
{
	const ComScope com;
	IWICImagingFactory *factory = nullptr;
	IWICStream *stream = nullptr;
	IWICBitmapDecoder *decoder = nullptr;
	IWICBitmapFrameDecode *frame = nullptr;
	IWICFormatConverter *conv = nullptr;
	bool ok = false;
	if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
		SUCCEEDED(factory->CreateStream(&stream)) &&
		SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE *>(png.data()), static_cast<DWORD>(png.size()))) &&
		SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
		SUCCEEDED(decoder->GetFrame(0, &frame))) {
		WICPixelFormatGUID fmt{};
		UINT w = 0, h = 0;
		frame->GetPixelFormat(&fmt);
		frame->GetSize(&w, &h);
		const bool deep = fmt == GUID_WICPixelFormat48bppRGB || fmt == GUID_WICPixelFormat64bppRGBA ||
			fmt == GUID_WICPixelFormat64bppRGB;
		const WICPixelFormatGUID to = deep ? GUID_WICPixelFormat48bppRGB : GUID_WICPixelFormat24bppBGR;
		const UINT stride = w * (deep ? 6u : 3u);
		if (SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
			SUCCEEDED(conv->Initialize(frame, to, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
			pixels->resize(static_cast<size_t>(stride) * h);
			ok = SUCCEEDED(conv->CopyPixels(nullptr, stride, static_cast<UINT>(pixels->size()), pixels->data()));
			*width = w;
			*height = h;
			*bits = deep ? 16u : 8u;
		}
	}
	safe_release(conv);
	safe_release(frame);
	safe_release(decoder);
	safe_release(stream);
	safe_release(factory);
	return ok;
}

bool read_shared_tail(const std::wstring &path, uint64_t max, std::string *out, bool *cut)
{
	out->clear();
	*cut = false;
	const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER size{};
	bool ok = GetFileSizeEx(f, &size) != 0;
	if (ok) {
		const uint64_t total = static_cast<uint64_t>(size.QuadPart);
		const uint64_t take = std::min(total, max);
		*cut = take < total;
		LARGE_INTEGER from{};
		from.QuadPart = static_cast<LONGLONG>(total - take);
		ok = SetFilePointerEx(f, from, nullptr, FILE_BEGIN) != 0;
		out->resize(static_cast<size_t>(take));
		size_t got = 0;
		while (ok && got < out->size()) {
			DWORD n = 0;
			const DWORD want = static_cast<DWORD>(std::min<size_t>(out->size() - got, 1u << 24));
			ok = ReadFile(f, out->data() + got, want, &n, nullptr) != 0;
			if (n == 0)
				break;
			got += n;
		}
		out->resize(got);
	}
	CloseHandle(f);
	return ok;
}

bool write_report_bundle(const ReportBundle &b, uint64_t *bytes, std::wstring *error, std::string *readme_out)
{
	*bytes = 0;
	SYSTEMTIME t{};
	GetLocalTime(&t);
	const uint16_t dtime = static_cast<uint16_t>((t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2));
	const uint16_t ddate = static_cast<uint16_t>(((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay);
	ZipBuilder zip;
	std::string readme = b.summary;
	if (!readme.empty() && readme.back() != '\n')
		readme += '\n';
	readme += "\nWhat is in this file:\n";
	const auto listed = [&readme](const std::string &name, const std::string &what) {
		readme += "  " + name + (what.empty() ? "" : " - " + what) + "\n";
	};
	bool fits = true;
	const auto add = [&](const std::string &name, const void *data, size_t size) {
		fits = zip.add(name, data, size, dtime, ddate) && fits;
	};

	for (const ReportFile &f : b.files) {
		std::string data;
		bool cut = false;
		if (!read_shared_tail(f.path, b.max_file_bytes, &data, &cut)) {
			listed(f.name, "not found at " + narrow(f.path));
			continue;
		}
		if (cut)
			data.insert(0, "[the start of this file is left out: only its last " + std::to_string(b.max_file_bytes) +
				" bytes are kept]\n");
		add(f.name, data.data(), data.size());
		listed(f.name, std::to_string(data.size()) + " bytes" + (cut ? ", the end of the file only" : ""));
	}
	add("flight.csv", b.flight_csv.data(), b.flight_csv.size());
	listed("flight.csv", "the flight recorder: one row a presented frame, the newest last (docs/diagnostics.md)");

	std::string frame_note = b.frame_note;
	if (!b.frame_dir.empty()) {
		const struct {
			const wchar_t *plane;
			const char *name;
		} planes[] = { { L"drawn", "before" }, { L"final", "after" } };
		for (const auto &p : planes) {
			std::vector<uint8_t> fmt, raw;
			const std::wstring fmt_path = b.frame_dir + L"\\" + p.plane + L".fmt";
			const std::wstring raw_path = b.frame_dir + L"\\f_0000_" + p.plane + L".bin";
			if (!read_all(fmt_path, &fmt) || !read_all(raw_path, &raw)) {
				listed(std::string(p.name) + ".png", "missing: the frame did not reach this stage");
				continue;
			}
			unsigned w = 0, h = 0, f = 0, rb = 0;
			const std::string fs(fmt.begin(), fmt.end());
			if (std::sscanf(fs.c_str(), "%u %u %u %u", &w, &h, &f, &rb) != 4 ||
				static_cast<uint64_t>(rb) * h > raw.size()) {
				listed(std::string(p.name) + ".png", "missing: the plane's format could not be read");
				continue;
			}
			std::vector<uint8_t> png;
			bool lossless = false;
			std::wstring why;
			char what[160]{};
			if (encode_png(raw.data(), w, h, rb, f, &png, &lossless, &why)) {
				add(std::string(p.name) + ".png", png.data(), png.size());
				std::snprintf(what, sizeof(what), "%ux%u, DXGI format %u, %s", w, h, f,
					lossless ? "lossless" : "clamped to 0..1 and sRGB-encoded, the exact plane is beside it");
				listed(std::string(p.name) + ".png", what);
			} else {
				listed(std::string(p.name) + ".png", "not made: " + narrow(why));
			}
			if (!lossless) {
				add(std::string(p.name) + ".bin", raw.data(), raw.size());
				add(std::string(p.name) + ".fmt", fmt.data(), fmt.size());
				listed(std::string(p.name) + ".bin", "the plane exactly, rows packed; " + std::string(p.name) +
					".fmt is \"width height dxgi_format row_bytes\"");
			}
		}
		for (const char *extra : { "trace.csv", "scope.txt" }) {
			std::vector<uint8_t> data;
			const std::wstring wide(extra, extra + std::strlen(extra));
			if (read_all(b.frame_dir + L"\\" + wide, &data)) {
				add(std::string("frame/") + extra, data.data(), data.size());
				listed(std::string("frame/") + extra, "the jitter scope's record of that frame (docs/jitter.md, \"Seeing it\")");
			}
		}
	} else {
		listed("before.png, after.png", "not taken: " + (frame_note.empty() ? std::string("no frame") : frame_note));
	}
	add("README.txt", readme.data(), readme.size());
	zip.finish();
	if (readme_out != nullptr)
		*readme_out = readme;
	if (!fits) {
		*error = L"the report would pass 4 GB";
		return false;
	}
	const HANDLE f = CreateFileW(b.zip_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		wchar_t m[64]{};
		_snwprintf_s(m, _TRUNCATE, L"could not create the file (error %lu)", GetLastError());
		*error = m;
		return false;
	}
	const std::vector<uint8_t> &data = zip.bytes();
	size_t done = 0;
	bool ok = true;
	while (ok && done < data.size()) {
		DWORD n = 0;
		const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 24));
		ok = WriteFile(f, data.data() + done, want, &n, nullptr) != 0 && n == want;
		done += n;
	}
	CloseHandle(f);
	if (!ok) {
		DeleteFileW(b.zip_path.c_str());
		*error = L"could not write the file";
		return false;
	}
	*bytes = data.size();
	return true;
}

}

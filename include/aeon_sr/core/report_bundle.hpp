#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aeon_sr {

uint32_t crc32_of(const void *data, size_t size, uint32_t crc = 0) noexcept;

class ZipBuilder {
public:
	bool add(const std::string &name, const void *data, size_t size, uint16_t dos_time, uint16_t dos_date);
	void finish();
	const std::vector<uint8_t> &bytes() const noexcept { return out_; }
	size_t entries() const noexcept { return entries_.size(); }

private:
	struct Entry {
		std::string name;
		uint32_t crc = 0, size = 0, offset = 0;
		uint16_t time = 0, date = 0;
	};
	std::vector<uint8_t> out_;
	std::vector<Entry> entries_;
	bool finished_ = false;
};

struct ZipEntryRead {
	std::string name;
	std::vector<uint8_t> data;
	bool crc_ok = false;
};
bool read_stored_zip(const std::vector<uint8_t> &zip, std::vector<ZipEntryRead> *out);

bool encode_png(const uint8_t *rows, uint32_t width, uint32_t height, uint32_t row_bytes, uint32_t dxgi_format,
	std::vector<uint8_t> *png, bool *lossless, std::wstring *error);
bool decode_png(const std::vector<uint8_t> &png, uint32_t *width, uint32_t *height, uint32_t *bits,
	std::vector<uint8_t> *pixels);

bool read_shared_tail(const std::wstring &path, uint64_t max, std::string *out, bool *cut);

inline constexpr uint64_t kReportMaxFileBytes = 16ull << 20;

struct ReportFile {
	std::string name;
	std::wstring path;
};

struct ReportBundle {
	std::wstring zip_path;
	std::vector<ReportFile> files;
	std::string flight_csv;
	std::wstring frame_dir;
	std::string frame_note;
	std::string summary;
	uint64_t max_file_bytes = kReportMaxFileBytes;
};

bool write_report_bundle(const ReportBundle &b, uint64_t *bytes, std::wstring *error, std::string *readme = nullptr);

}

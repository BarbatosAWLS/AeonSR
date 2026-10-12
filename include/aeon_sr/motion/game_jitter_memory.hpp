#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aeon_sr {

class GameJitterMemory {
public:
	static constexpr size_t kMaxEntries = 256u;

	struct Entry {
		std::string exe;
		uint32_t w = 0, h = 0;
		bool jitters = false;
	};

	static std::string key_of(std::string exe)
	{
		for (char &c : exe) {
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
			else if (c == '/')
				c = '\\';
		}
		return exe;
	}

	bool find(const std::string &exe, uint32_t w, uint32_t h, bool *jitters) const
	{
		const std::string k = key_of(exe);
		for (const Entry &e : entries_) {
			if (e.w == w && e.h == h && e.exe == k) {
				*jitters = e.jitters;
				return true;
			}
		}
		return false;
	}

	bool remember(const std::string &exe, uint32_t w, uint32_t h, bool jitters)
	{
		if (exe.empty() || w == 0 || h == 0)
			return false;
		const std::string k = key_of(exe);
		for (size_t i = 0; i < entries_.size(); ++i) {
			if (entries_[i].w != w || entries_[i].h != h || entries_[i].exe != k)
				continue;
			if (entries_[i].jitters == jitters)
				return false;
			entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
			break;
		}
		entries_.push_back({ k, w, h, jitters });
		if (entries_.size() > kMaxEntries)
			entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - kMaxEntries));
		return true;
	}

	void parse(const std::string &text)
	{
		entries_.clear();
		size_t at = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3u : 0u;
		while (at < text.size()) {
			size_t end = text.find('\n', at);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(at, end - at);
			at = end + 1;
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			Entry e;
			if (read_line(line, &e))
				(void)remember(e.exe, e.w, e.h, e.jitters);
		}
	}

	std::string text() const
	{
		std::string out;
		for (const Entry &e : entries_)
			out += std::string(e.jitters ? "1 " : "0 ") + std::to_string(e.w) + "x" + std::to_string(e.h) + " " + e.exe + "\n";
		return out;
	}

	const std::vector<Entry> &entries() const noexcept { return entries_; }

private:
	static bool read_number(const std::string &s, size_t *at, char stop, uint32_t *out)
	{
		uint64_t v = 0;
		const size_t from = *at;
		while (*at < s.size() && s[*at] >= '0' && s[*at] <= '9' && *at - from < 6u)
			v = v * 10u + static_cast<uint64_t>(s[(*at)++] - '0');
		if (*at == from || *at >= s.size() || s[*at] != stop || v == 0)
			return false;
		++*at;
		*out = static_cast<uint32_t>(v);
		return true;
	}

	static bool read_line(const std::string &line, Entry *e)
	{
		if (line.size() < 7u || (line[0] != '0' && line[0] != '1') || line[1] != ' ')
			return false;
		size_t at = 2;
		if (!read_number(line, &at, 'x', &e->w) || !read_number(line, &at, ' ', &e->h) || at >= line.size())
			return false;
		e->jitters = line[0] == '1';
		e->exe = line.substr(at);
		return true;
	}

	std::vector<Entry> entries_;
};

bool load_game_jitter_memory(const std::wstring &path, GameJitterMemory *out);
bool save_game_jitter_memory(const std::wstring &path, const GameJitterMemory &memory);
std::string game_jitter_exe_path();

}

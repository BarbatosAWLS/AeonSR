#include "aeon_sr/motion/game_jitter_memory.hpp"

#include <windows.h>

#include <cstdio>
#include <iterator>

namespace aeon_sr {

bool load_game_jitter_memory(const std::wstring &path, GameJitterMemory *out)
{
	FILE *f = nullptr;
	if (path.empty() || _wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
		return false;
	std::string text;
	char buf[4096];
	size_t n = 0;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < (1u << 20))
		text.append(buf, n);
	std::fclose(f);
	out->parse(text);
	return true;
}

bool save_game_jitter_memory(const std::wstring &path, const GameJitterMemory &memory)
{
	if (path.empty())
		return false;
	const size_t slash = path.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
		CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
	const std::wstring tmp = path + L".tmp";
	FILE *f = nullptr;
	if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || f == nullptr)
		return false;
	const std::string text = memory.text();
	const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	const bool closed = std::fclose(f) == 0;
	if (!written || !closed) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		return true;
	DeleteFileW(tmp.c_str());
	const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD wrote = 0;
	const bool ok = SetFilePointer(h, 0, nullptr, FILE_BEGIN) == 0 && SetEndOfFile(h) &&
		WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size() &&
		FlushFileBuffers(h);
	CloseHandle(h);
	return ok;
}

std::string game_jitter_exe_path()
{
	wchar_t wide[MAX_PATH * 4];
	const DWORD n = GetModuleFileNameW(nullptr, wide, static_cast<DWORD>(std::size(wide)));
	if (n == 0 || n >= std::size(wide))
		return {};
	const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
	if (bytes <= 0)
		return {};
	std::string out(static_cast<size_t>(bytes), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(n), out.data(), bytes, nullptr, nullptr);
	return out;
}

}

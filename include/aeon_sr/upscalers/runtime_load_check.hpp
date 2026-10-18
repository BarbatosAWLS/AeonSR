#pragma once

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>

namespace aeon_sr {

namespace runtime_load_detail {

inline std::wstring module_path(HMODULE module)
{
	std::wstring buf(MAX_PATH, L'\0');
	for (int tries = 0; tries < 4; ++tries) {
		const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
		if (n == 0)
			return std::wstring();
		if (n < buf.size()) {
			buf.resize(n);
			return buf;
		}
		buf.resize(buf.size() * 2);
	}
	return std::wstring();
}

inline bool same_file(const std::wstring &a, const std::wstring &b)
{
	if (a.empty() || b.empty())
		return false;
	if (CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL)
		return true;
	const auto identity = [](const std::wstring &path, BY_HANDLE_FILE_INFORMATION *out) {
		const HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		const bool ok = GetFileInformationByHandle(h, out) != FALSE;
		CloseHandle(h);
		return ok;
	};
	BY_HANDLE_FILE_INFORMATION x{}, y{};
	return identity(a, &x) && identity(b, &y) && x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
		x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
}

inline std::atomic<bool> &redirected_flag() noexcept
{
	static std::atomic<bool> flag{ false };
	return flag;
}

}

inline bool loaded_module_is(HMODULE module, const std::wstring &asked, std::wstring *got = nullptr)
{
	const std::wstring path = runtime_load_detail::module_path(module);
	if (path.empty() || runtime_load_detail::same_file(path, asked))
		return true;
	if (got != nullptr)
		*got = path;
	runtime_load_detail::redirected_flag().store(true, std::memory_order_relaxed);
	return false;
}

namespace runtime_load_detail {

inline bool starts_with_jump(const unsigned char *code) noexcept
{
	if (code[0] == 0xE9 || code[0] == 0xEB)
		return true;
	if (code[0] == 0xFF && code[1] == 0x25)
		return true;
	if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0)
		return true;
	if (code[0] == 0x49 && code[1] == 0xBB && code[10] == 0x41 && code[11] == 0xFF && code[12] == 0xE3)
		return true;
	return false;
}

inline bool file_bytes_at_rva(const std::wstring &path, DWORD rva, unsigned char out[16])
{
	const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	const auto read_at = [h](LONGLONG offset, void *dst, DWORD size) {
		LARGE_INTEGER li;
		li.QuadPart = offset;
		DWORD got = 0;
		return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, dst, size, &got, nullptr) && got == size;
	};
	bool ok = false;
	IMAGE_DOS_HEADER dos{};
	IMAGE_NT_HEADERS nt{};
	if (read_at(0, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
		read_at(dos.e_lfanew, &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE) {
		const LONGLONG sections = dos.e_lfanew + offsetof(IMAGE_NT_HEADERS, OptionalHeader) +
			nt.FileHeader.SizeOfOptionalHeader;
		for (WORD i = 0; i < nt.FileHeader.NumberOfSections && !ok; ++i) {
			IMAGE_SECTION_HEADER s{};
			if (!read_at(sections + i * static_cast<LONGLONG>(sizeof(s)), &s, sizeof(s)))
				break;
			if (rva >= s.VirtualAddress && rva + 16u <= s.VirtualAddress + s.SizeOfRawData)
				ok = read_at(static_cast<LONGLONG>(s.PointerToRawData) + (rva - s.VirtualAddress), out, 16);
		}
	}
	CloseHandle(h);
	return ok;
}

}

inline bool export_patched(HMODULE module, const char *name)
{
	const auto *fn = reinterpret_cast<const unsigned char *>(GetProcAddress(module, name));
	if (fn == nullptr || !runtime_load_detail::starts_with_jump(fn))
		return false;
	const std::wstring path = runtime_load_detail::module_path(module);
	unsigned char disk[16] = {};
	const DWORD rva = static_cast<DWORD>(fn - reinterpret_cast<const unsigned char *>(module));
	if (path.empty() || !runtime_load_detail::file_bytes_at_rva(path, rva, disk))
		return false;
	if (memcmp(disk, fn, 5) == 0)
		return false;
	runtime_load_detail::redirected_flag().store(true, std::memory_order_relaxed);
	return true;
}

inline bool runtime_load_redirected() noexcept
{
	return runtime_load_detail::redirected_flag().load(std::memory_order_relaxed);
}

inline void reset_runtime_load_redirected() noexcept
{
	runtime_load_detail::redirected_flag().store(false, std::memory_order_relaxed);
}

inline std::wstring runtime_hooked_error(const std::wstring &path, const char *name)
{
	return L"another program in this game hooked " + std::wstring(name, name + strlen(name)) + L" in " + path +
		L" (OptiScaler does this to take a runtime's calls over), so Aeon SR does not use it in the game's "
		L"process; its upscalers move to AeonSRHost.exe, apart from it";
}

inline std::wstring runtime_load_redirect_error(const std::wstring &asked, const std::wstring &got)
{
	return L"another program in this game handed the load of " + asked + L" a different file, " + got +
		L" (OptiScaler does this to take a runtime's calls over), so Aeon SR does not use it in the game's "
		L"process; its upscalers move to AeonSRHost.exe, apart from it";
}

template <typename Fn>
bool resolve_export(HMODULE module, const char *name, Fn *&out, std::wstring &error)
{
	out = reinterpret_cast<Fn *>(GetProcAddress(module, name));
	if (out == nullptr) {
		error = L"GetProcAddress failed for ";
		error += std::wstring(name, name + strlen(name));
		return false;
	}
	return true;
}

}
